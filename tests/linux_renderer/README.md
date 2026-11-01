# DXVK Native 2.7.1 D3D9 + SDL3 experiments (32-bit, private)

This directory holds three separate things, and they have different requirements:

| Part | What it is | Needs | Runs in CI |
| --- | --- | --- | --- |
| `d3d9_sdl3_smoke` | device, swap chain, resize and reset smoke | real GPU, DXVK Native 2.7.1, SDL3 | no |
| asset-free checks | CPU tests of the bundle, oracle and capture helpers | 32-bit C++20 compiler | yes, `world_surface` job |
| `world_surface_replay`, `world_lit_replay` | replay one exported surface through the same device | real GPU, DXVK Native 2.7.1, SDL3, a bundle built outside the repository | no |

Everything here is an experiment on the machine that ran it. A pass says the
particular path exercised on that machine works, nothing more.

Scope and isolation:

- The smoke test includes only `<d3d9.h>`, `<windows.h>`, `<wsi/native_sdl3.h>`
  and SDL3/Vulkan. No engine headers (`compat/`, `q_shared`, renderer, world) and
  no assets.
- The two replays do link three engine translation units, because the IWI decoder
  and the pixel decode are engine code (see [The replay targets](#the-replay-targets-gpu--private-bundle)).
  They still include no engine header from their own DXVK/SDL3 translation unit,
  and they load no assets: every input is a bundle passed on the command line.
- The CMake project in this directory is standalone. The repository's root
  `CMakeLists.txt` does not add it (`add_subdirectory` reaches
  `scripts/platform/linux`, `scripts/mp`, `scripts/sp`, `scripts/dedi` and
  `scripts/radiant` only), and no root target builds these files.
- No CI job configures this CMake project, so neither the smoke nor either replay
  is built or run on hosted runners. What CI does run is the asset-free checks,
  through `scripts/tests/run_world_surface_checks.py`, which compiles a few
  headers and one `src/linux/tools` unit directly and never involves CMake.
- Nothing game-derived is committed. Bundles, IWI textures and PPM captures are
  produced outside the repository from a licensed installation, and the unlit
  replay refuses to write its output inside the checkout.

## Prerequisites

| Item | Location used for the first run |
| --- | --- |
| DXVK Native 2.7.1 install | `/tmp/kisak-dxvk-prefix` (`include/dxvk`, `lib32`, `lib32/pkgconfig/dxvk-d3d9.pc`) |
| DXVK source revision | `c3dd74be6baec53786d4e064a572185b70347a17` (also the source of `include/vulkan/include`) |
| SDL3 SDK (3.2.10, i386) | `/tmp/kisak-sdl3-sdk/root` (`usr/include`, `usr/lib/i386-linux-gnu`) |
| Vulkan headers | `<dxvk-source>/include/vulkan/include` or any Vulkan SDK |
| Toolchain | `g++ -m32` for the smoke and the asset-free checks, plus a 32-bit Vulkan loader/ICD and 32-bit SDL3 runtime |

The DXVK native build must include the SDL3 window system integration
(`-Dnative_sdl3=enabled` at configure time). Its pkg-config module is `dxvk-d3d9` and
`Cflags` already points at `<prefix>/include/dxvk`, so `<d3d9.h>` and
`<wsi/native_sdl3.h>` resolve with `-I$(pkg-config --cflags-only-I dxvk-d3d9)`.

Both replays configure the decoder object library for the engine build
(`KISAK_MP KISAK_OPENAL RELEASE_ASSERTS _FILE_OFFSET_BITS=64 _GNU_SOURCE
KISAK_LINUX_SDL_MAJOR=3`), so they need the repository's `src` and `deps` trees.

Note on the SDK layout: the staged SDL3 SDK's `sdl3.pc` says `prefix=/usr`, so pkg-config
only turns its paths into staged locations with `PKG_CONFIG_SYSROOT_DIR`. That variable
would also rewrite the DXVK prefix, so this project never sets it. Use explicit `-I`/`-L`
(the manual command below) or `-DKDXK_SDL3_SDK_ROOT=/tmp/kisak-sdl3-sdk/root`.

## Build the smoke

Manual command (no pkg-config involvement, matches the first verified run):

```sh
g++ -m32 -std=c++17 -O2 -Wall -Wextra \
  -I/tmp/kisak-dxvk-prefix/include/dxvk \
  -I/tmp/kisak-sdl3-sdk/root/usr/include \
  -I/tmp/kisak-dxvk-source/include/vulkan/include \
  tests/linux_renderer/d3d9_sdl3_smoke.cpp \
  -L/tmp/kisak-dxvk-prefix/lib32 \
  -L/tmp/kisak-sdl3-sdk/root/usr/lib/i386-linux-gnu \
  -ldxvk_d3d9 -lSDL3 \
  -Wl,-rpath-link,/tmp/kisak-sdl3-sdk/root/usr/lib/i386-linux-gnu \
  -o build/linux_renderer_smoke/d3d9_sdl3_smoke
```

`-Wl,-rpath-link,...` lets the linker resolve transitive dependencies in the
private SDK. The parent build passed without suppressing unresolved-symbol
errors: the link line carries no `-Wl,--allow-shlib-undefined` and no
`-Wl,--unresolved-symbols=ignore-all`, so a genuinely unresolved symbol still
fails the link. Runtime dependencies must still be present and match the i386 ABI.

CMake variant (pkg-config resolves `dxvk-d3d9`, SDL3 comes from the SDK root):

```sh
PKG_CONFIG_PATH=/tmp/kisak-dxvk-prefix/lib32/pkgconfig \
cmake -S tests/linux_renderer -B build/linux_renderer_smoke \
      -DCMAKE_CXX_FLAGS=-m32 -DCMAKE_EXE_LINKER_FLAGS=-m32 \
      -DKDXK_SDL3_SDK_ROOT=/tmp/kisak-sdl3-sdk/root \
      -DKDXK_VULKAN_INCLUDE_DIR=/tmp/kisak-dxvk-source/include/vulkan/include
cmake --build build/linux_renderer_smoke
```

That configures with both replay options off and builds the smoke plus the two
device-free test targets. The project refuses to configure unless the compiler
produces 32-bit binaries, and it checks at configure time that `d3d9.h`,
`d3d9types.h`, `d3d9caps.h`, `windows.h` and `wsi/native_sdl3.h` exist in the
`dxvk-d3d9` include directories.

## Run the smoke

```sh
LD_LIBRARY_PATH=/tmp/kisak-dxvk-prefix/lib32:/tmp/kisak-sdl3-sdk/root/usr/lib/i386-linux-gnu \
DXVK_WSI_DRIVER=SDL3 \
build/linux_renderer_smoke/d3d9_sdl3_smoke --frames 180 --resize-at 60 --reset-at 120
```

`DXVK_WSI_DRIVER` must be `SDL3` (DXVK's SDL3 WSI is registered under exactly that name
and DXVK has no Linux fallback). If the variable is unset the smoke sets it to `SDL3` for
its own process and says so; if it is set to anything else the run stops immediately.
`LD_LIBRARY_PATH` is needed twice over: for the dynamic linker, and because DXVK inside
`libdxvk_d3d9.so` `dlopen()`s `libSDL3.so.0` itself.

### Options

| Option | Default | Meaning |
| --- | --- | --- |
| `--frames N` | 180 | frames to present before exiting |
| `--resize-at N` | 60 | frame that resizes the window, then resets the device |
| `--reset-at N` | 120 | frame that resets the device again at the unchanged size |
| `--width/--height N` | 640x360 | initial window size in points |
| `--resize-width/--resize-height N` | 800x480 | window size requested at `--resize-at` |
| `--expect-gpu TEXT` | empty | require this substring in the Vulkan `deviceName`; CPU devices are always rejected |
| `--help` | | usage text |

`--expect-gpu` can also come from `KDXK_SMOKE_EXPECT_GPU`. The run rejects
`--resize-at` below 2 (at least one initial-stage frame must be presented) and a resize
target equal to the initial size (a no-op resize proves nothing).

### Exit codes

| Code | Meaning |
| --- | --- |
| 0 | pass: every check ran and passed |
| 1 | failure: an HRESULT, a check, or the GPU filter failed (stage is printed) |
| 2 | usage error |
| 3 | Vulkan unsupported: no loader, no instance, no physical device, or CPU devices only |
| 4 | interrupted: window closed or Escape pressed before the frame budget ended |

## What the smoke checks

1. Window on the main thread, visible, created with `SDL_WINDOW_VULKAN | SDL_WINDOW_RESIZABLE`,
   converted to a DXVK native `HWND` through `dxvk::wsi::toHwnd`.
2. Vulkan preflight: enumerate physical devices through the loader SDL opened, log every
   `deviceName` and `VkPhysicalDeviceType`, reject CPU devices, honour `--expect-gpu`.
   This is the authoritative GPU statement: DXVK D3D9 hides or spoofs the real GPU in
   `IDirect3D9::GetAdapterIdentifier` (on the reference machine an Intel integrated GPU
   is reported as "AMD Radeon RX 6700 XT"), so the adapter string is logged but not
   trusted.
3. `Direct3DCreate9(D3D_SDK_VERSION)`, then `CreateDevice(D3DDEVTYPE_HAL,
   D3DCREATE_HARDWARE_VERTEXPROCESSING)` with `X8R8G8B8`, `D3DSWAPEFFECT_DISCARD`, one
   back buffer, windowed, vsync interval.
4. Back buffer description via `GetBackBuffer` + `GetDesc` compared with the pixel size
   SDL reports, before the resize.
5. At `--resize-at`: request the resize, poll a bounded number of times (60 polls of 5 ms)
   for `SDL_GetWindowSizeInPixels` to report the expected pixel size, then reset the
   device with a freshly built `D3DPRESENT_PARAMETERS`, and compare the swap chain
   parameters and the new back buffer with that size again.
6. At `--reset-at`: confirm the pixel size has not drifted, reset again at the unchanged
   size, and repeat the swap chain and back buffer comparisons.
7. Clear (`D3DCLEAR_TARGET`) and `Present` every frame, with a per-stage colour
   (green before the resize, amber after the resize, blue after the same-size reset).
8. Teardown in a fixed order: `IDirect3DDevice9`, `IDirect3D9`, window, `SDL_Quit`.

The summary prints frame/present/reset/resize/back-buffer counts, passed and failed
checks, and event counts. Any HRESULT failure prints `[fail] stage=... hr=0x...` and exits
non-zero.

A reference run looked like this:

```
[vulkan] device 0: "Intel(R) Graphics (RPL-U)" type=integrated-gpu  <- selected
[vulkan] device 1: "llvmpipe (LLVM 19.1.7, 256 bits)" type=cpu
[check] resize-reset-backbuffer  OK   back buffer 800x480 format=22 pool=0, window pixels 800x480
[check] same-size-reset-backbuffer OK back buffer 800x480 format=22 pool=0, window pixels 800x480
[result] PASS: 12 frame(s) presented, 22 check(s) passed
[summary] frames=12 clears=12 presents=12 resets=2
```

## No-pass rules

- No Vulkan loader, no instance, no physical device, or CPU (`llvmpipe`/lavapipe) devices
  only: exit code 3 and a "not a pass" line. A software rasteriser result must never be
  recorded as a pass for this experiment.
- A requested GPU that is not enumerated: exit code 1.
- No Wine, no DXVK on Windows, and no substituted renderer counts as a result here.
- An interrupted run (window closed, Escape) is not a pass either.

## Asset-free checks (no DXVK, no SDL, no device)

These are the checks CI runs. They need neither the CMake project above nor any
graphics dependency, because they are compiled directly:

```sh
python3 scripts/tests/run_world_surface_checks.py --config Debug
python3 scripts/tests/run_world_surface_checks.py --config RelWithDebInfo
python3 scripts/tests/run_world_surface_checks.py --list   # print the commands, run nothing
```

The runner performs no CMake configure and no engine build. It compiles six test
translation units with `-m32 -std=c++20 -Werror` and runs them, then runs the
bundle prepare script, for seven checks in total:

| Check | Source | Covers |
| --- | --- | --- |
| `world_surface_inventory` | `tests/linux_asset_export/world_surface_inventory_test.cpp` | inventory allowlist, declaration routing, slice bounds, bundle helper arithmetic |
| `world_surface_context` | `world_surface_context_test.cpp` | shader-constant context for one unlit surface draw |
| `world_surface_bundle` | `world_surface_bundle_test.cpp` | bundle loader, shader framing walk, vertex accessors |
| `world_surface_oracle` | `world_surface_oracle_test.cpp` | oracle hashing, weights, sampling, colour and pixel arithmetic, sample selection |
| `world_lit_bundle` | `world_lit_bundle_test.cpp` | lit loader refusals: program pair hash and length, widened geometry counts, index walk, colour-map sampler, exact constant set, lightmap layout |
| `world_image_capture` | `tests/linux_asset_export/world_image_capture_test.cpp` | capture bounds, name filter, duplicate rule, disabled path |
| `world_surface_prepare` | `scripts/tests/test_world_surface_prepare.py` | the prepare step's trust boundary over archives it writes itself |

`world_image_capture` is the only case with a second translation unit: the capture
state lives behind a mutex in `src/linux/tools/world_image_capture.cpp`, so that
unit is compiled beside the test and linked with `-pthread`. It is still
engine-free. Every other case is a single translation unit that includes one
header and opens no file except bundles it writes itself.

Configurations are explicit, not inherited from a cache: `Debug` is `-O0` and
`RelWithDebInfo` is `-O2`, both with `-g`. Each command runs in its own process
group with its own timeout. A check only passes if the process exits 0, the
binary really is ELF32/i386, and its output contains no `FAIL` line.

What a passing run does not claim: it does not claim that a surface was rendered,
that a capture was reproduced, or that the hosted workflow itself ran. The checks
verify the helpers against their own stated behaviour.

GitHub Actions runs this runner in both configurations, in the `world_surface` job
of `.github/workflows/build-kisakcod-linux.yaml`. It installs only `g++`,
`g++-multilib` and Python; there is no DXVK, SDL, GPU or asset step.

`world_surface_context_test.cpp`, `world_surface_inventory_test.cpp` and
`world_image_capture_test.cpp` have no target here. They are compiled only by this
runner, which is why the CMake project below lists fewer test targets than the
runner has checks.

## The replay targets (GPU + private bundle)

Two opt-in executables replay one exported world surface through the same D3D9
device as the smoke, and compare a bounded pixel readback against a CPU oracle:

| Target | Option | Profile |
| --- | --- | --- |
| `world_surface_replay` | `KDXK_WORLD_SURFACE_REPLAY` | unlit: three declaration elements, material colour at constant code 40, base map on sampler 0 |
| `world_lit_replay` | `KDXK_WORLD_LIT_REPLAY` | lit: four declaration elements, normal `UBYTE4` at offset 36, codes 60/76/41/42 and no 40, base map on s0 plus the secondary lightmap on s3 |

A distinct executable per profile, because the declaration, the constant set and
the sampler closure all differ. They share only the build inputs.

The engine's IWI decoder (`src/linux/client/window/iwi_assets.cpp`) and the pixel
decode it calls (`src/gfx_d3d/r_imagedecode.cpp`) cannot compile in the replay's
own translation unit: they reach `r_image.h`, which pulls `xanim` -> `sound` ->
`deps/msslib/mss.h`, and those decompiled headers need the engine's defines and
`-fms-extensions`. So the decoder is built once into an object library with
exactly the engine's flags, and each replay links those objects; the DXVK/SDL3
translation unit never sees an engine include root, only `iwi_assets.h`. The
decoder objects are compiled with `-ffunction-sections -fdata-sections` and linked
with `-Wl,--gc-sections`, so the engine's file-loading decode entry points (which
traverse the whole engine runtime) are dropped instead of dragging `FS_ReadFile`
and `Com_Error` into the binary.

Configure both, with clang:

```sh
PKG_CONFIG_PATH=/tmp/kisak-dxvk-prefix/lib32/pkgconfig \
cmake -S tests/linux_renderer -B build/linux_renderer_replay \
      -DCMAKE_CXX_COMPILER=clang++-19 \
      -DCMAKE_CXX_FLAGS=-m32 -DCMAKE_EXE_LINKER_FLAGS=-m32 \
      -DKDXK_SDL3_SDK_ROOT=/tmp/kisak-sdl3-sdk/root \
      -DKDXK_VULKAN_INCLUDE_DIR=/tmp/kisak-dxvk-source/include/vulkan/include \
      -DKDXK_ENGINE_SOURCE_DIR="$PWD/src" \
      -DKDXK_WORLD_SURFACE_REPLAY=ON \
      -DKDXK_WORLD_LIT_REPLAY=ON
cmake --build build/linux_renderer_replay
```

`KDXK_ENGINE_SOURCE_DIR` is required once a replay is enabled, and GCC is refused
at configure time: the decompiled `GfxViewInfo`/`GfxBackEndData` declarations use
`const struct X { ... };`, which GCC rejects. `KDXK_WORLD_LIT_REPLAY` needs
`KDXK_WORLD_SURFACE_REPLAY=ON` as well, because it reuses the decoder object
library rather than building a second one.

Run the accepted unlit baseline (prepare and verify surface 249 and its genuine
colormap outside the checkout first; paths below are examples):

```sh
LD_LIBRARY_PATH=/tmp/kisak-dxvk-prefix/lib32:/tmp/kisak-sdl3-sdk/root/usr/lib/i386-linux-gnu \
DXVK_WSI_DRIVER=SDL3 \
build/linux_renderer_replay/world_surface_replay \
  --bundle /tmp/kisak-surface/export.json \
  --material-color 1,1,1,0 \
  --texture /tmp/kisak-surface/images/me_concrete_dest02_col.iwi \
  --tex-filter-disable 1 \
  --output /tmp/kisak-world-surface-replay/world-surface.ppm \
  --report /tmp/kisak-world-surface-replay/report.txt
```

Run the lit replay:

```sh
LD_LIBRARY_PATH=/tmp/kisak-dxvk-prefix/lib32:/tmp/kisak-sdl3-sdk/root/usr/lib/i386-linux-gnu \
DXVK_WSI_DRIVER=SDL3 \
build/linux_renderer_replay/world_lit_replay \
  --bundle /tmp/kisak-lit-export/world-lit-surface-bundle.json \
  --texture /tmp/kisak-lit-textures/me_wire_black.iwi \
  --fog-color 0,0,0,0 \
  --output /tmp/kisak-world-lit-replay/world-lit.ppm \
  --report /tmp/kisak-world-lit-replay/report.txt
```

Both set `DXVK_WSI_DRIVER=SDL3` for their own process when it is unset, like the
smoke. `--output` defaults to `/tmp/kisak-world-surface-replay/world-surface.ppm`
and `/tmp/kisak-world-lit-replay/world-lit.ppm` respectively.

### Required inputs, and what each program refuses

The two profiles take different inputs because the bundles carry different state:

| Input | `world_surface_replay` | `world_lit_replay` |
| --- | --- | --- |
| `--bundle` | required | required |
| `--material-color R,G,B,A` | **required** — constant code 40. The bundle records only a `colorTint` literal with no register base, so the run states the value and prints it. Alpha is a real input: the pixel shader lerps a replacement alpha through it, so `1,1,1,1` and `1,1,1,0` give different images. | not accepted: the lit profile has no code 40 |
| `--fog-color R,G,B,A` | not accepted | **required** — constant code 42, scene state the bundle does not carry, and the program will not invent it |
| `--texture PATH` | optional, but a real pass needs it: without it the run binds a 1x1 white texel and reports that the result is not textured | required, bound to sampler 0 |
| sampler filter inputs | **required**: pass `--tex-filter-disable 1` (values fixed by `r_state.cpp`) or the explicit `--mip-filter-mode`/`--anisotropic-filter`/`--aniso2x`/`--aniso4x`/`--linear-mipped-anisotropy`/`--linear-mipped-filter`/`--linear-nonmipped-filter` set. An all-zero default would silently pick a POINT/POINT sampler, which is a rendering choice the program is not entitled to make. | not needed |
| `--output PATH` | refused when the path names the checkout: no capture is committed to the repository | defaults outside the repository |
| `--camera-eye/--camera-target`, `--tan-half-fov`, `--camera-side`, `--camera-distance` | optional; the explicit eye/target pair is the only way to replay a surface with more than one triangle, and one flag without the other is refused | `--camera-eye/--camera-target`, `--tan-half-fov` |
| `--lightmap-perturb`, `--base-perturb` | — | optional control deltas |
| `--cpu-only` | — | loader and CPU prediction only, no device |
| `--expect-gpu NAME` | not accepted | require a Vulkan `deviceName` substring |
| `--self-test` | cull/sampler decode self-checks, no bundle and no device | — |

Neither program has an option that skips its controls: `--skip-controls` is
explicitly refused, because a control set that can be switched off would be
switched off and the run would still print `PASS`.

Exit codes:

| Code | `world_surface_replay` | `world_lit_replay` |
| --- | --- | --- |
| 0 | pass | pass |
| 1 | failure | failure |
| 2 | usage | usage |
| 3 | bundle invalid | bundle invalid |
| 4 | state block unsupported | state unsupported |
| 5 | device unavailable | device unavailable |
| 6 | — | prediction mismatch |
| 7 | — | not observable |
| 8 | — | texture invalid |

A readback is compared only against pixels selected from the bundle, the camera
and the two images, so nothing about the readback decides which pixels are
compared. When too few interior pixels can be sampled the lit replay fails with a
message asking for a different camera instead of lowering its sample floor.

## CMake options and targets

| Option | Default | Meaning |
| --- | --- | --- |
| `KDXK_SDL3_SDK_ROOT` | empty | staged SDL3 SDK root (`usr/include`, `usr/lib/i386-linux-gnu`) instead of the `sdl3` pkg-config module |
| `KDXK_VULKAN_INCLUDE_DIR` | found | directory containing `vulkan/vulkan.h`; not shipped by the DXVK install |
| `KDXK_ENGINE_SOURCE_DIR` | empty | the repository's `src`; required once a replay is enabled |
| `KDXK_WORLD_SURFACE_REPLAY` | `OFF` | build `world_surface_replay` and the decoder object library |
| `KDXK_WORLD_LIT_REPLAY` | `OFF` | build `world_lit_replay` and `world_lit_bundle_test`; needs `KDXK_WORLD_SURFACE_REPLAY=ON` |

| Target | Built | Needs |
| --- | --- | --- |
| `d3d9_sdl3_smoke` | always | `dxvk-d3d9`, SDL3, Vulkan headers |
| `world_surface_bundle_test` | always | nothing but the C++ compiler |
| `world_surface_oracle_test` | always | nothing but the C++17 compiler |
| `world_surface_replay` | `KDXK_WORLD_SURFACE_REPLAY=ON` | `dxvk-d3d9`, SDL3, Vulkan headers, engine `src`/`deps`, clang |
| `world_lit_bundle_test` | `KDXK_WORLD_LIT_REPLAY=ON` | nothing but the C++20 compiler (it sits inside the lit option's block) |
| `world_lit_replay` | `KDXK_WORLD_LIT_REPLAY=ON` | as `world_surface_replay` |

The two device-free test targets built here are the same sources the CI runner
compiles; they exist as targets so a developer with the SDKs can build them with
the rest.

## Troubleshooting

- `dxvk-d3d9` not found: point `PKG_CONFIG_PATH` at
  `/tmp/kisak-dxvk-prefix/lib32/pkgconfig`; the module lives next to `libdxvk_d3d9.so`.
- `vulkan/vulkan.h` not found: add
  `-I/tmp/kisak-dxvk-source/include/vulkan/include`. The DXVK install ships D3D9 headers
  only.
- `libdxvk_d3d9.so.0: cannot open shared object file`: set `LD_LIBRARY_PATH` to
  `/tmp/kisak-dxvk-prefix/lib32` and the SDL3 SDK's `usr/lib/i386-linux-gnu`.
- `Failed to initialize WSI` in `d3d9.log`: `DXVK_WSI_DRIVER` was not `SDL3`, or the DXVK
  build has no SDL3 window system integration (`native_sdl3` disabled), or `libSDL3.so.0`
  is not reachable through the loader path.
- `CreateDevice failed` with a Vulkan driver error: the ICD rejects something DXVK needs;
  `d3d9.log` in the working directory has the device and extension details.
- Resize never confirms: the window manager refused the geometry. The run stops after the
  bounded poll budget instead of resetting to a guessed size.
- A replay refuses to configure: `KDXK_ENGINE_SOURCE_DIR` is unset, or the compiler is
  GCC, or `KDXK_WORLD_LIT_REPLAY` was enabled without `KDXK_WORLD_SURFACE_REPLAY`.
- A replay rejects `--material-color`/`--fog-color`: they are required per profile and are
  not read from the bundle. See the table above.

## Files

| File | Purpose |
| --- | --- |
| `d3d9_sdl3_smoke.cpp` | the device/swap-chain/resize/reset smoke |
| `world_surface_bundle.h` | schema-1 unlit bundle reader, shader framing walk, vertex accessors |
| `world_surface_bundle_test.cpp` | its refusals and arithmetic |
| `world_surface_context.h` | shader-constant context for one unlit surface draw |
| `world_surface_context_test.cpp` | its arithmetic (compiled by the CI runner only) |
| `world_surface_oracle.h` | the CPU prediction a replay readback is compared against |
| `world_surface_oracle_test.cpp` | its hashing, weights, sampling, colour and pixel arithmetic |
| `world_surface_replay.cpp` | the unlit replay executable |
| `world_lit_bundle.h` | the lit bundle reader; reuses `world_surface_bundle.h` and adds no graphics header |
| `world_lit_bundle_test.cpp` | the lit loader's refusals |
| `world_lit_replay.cpp` | the lit replay executable |
| `CMakeLists.txt` | standalone project: pkg-config `dxvk-d3d9`, SDL3 from pkg-config or `KDXK_SDL3_SDK_ROOT`, two opt-in replay options |
| `README.md` | this document |
