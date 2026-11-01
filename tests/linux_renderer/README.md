# DXVK Native 2.7.1 D3D9 + SDL3 smoke (32-bit, private experiment)

Standalone smoke test for the 32-bit DXVK Native D3D9 build with the SDL3 window system
integration. It creates one window, one D3D9 device, clears and presents a bounded number
of frames, resizes the window once (with a device reset), resets once more at the
unchanged size, and reports a verdict with counted checks.

Scope and isolation:

- Only `<d3d9.h>`, `<windows.h>`, `<wsi/native_sdl3.h>` and SDL3/Vulkan are included. No
  engine headers (`compat/`, `q_shared`, renderer, world) and no assets.
- The CMake project in this directory is standalone. The repository's root
  `CMakeLists.txt` does not add it, and no CI job runs it.
- This is an initial device experiment. A pass here says the D3D9 device, swap chain,
  resize and reset paths work on the machine that ran it, nothing more.

## Prerequisites

| Item | Location used for the first run |
| --- | --- |
| DXVK Native 2.7.1 install | `/tmp/kisak-dxvk-prefix` (`include/dxvk`, `lib32`, `lib32/pkgconfig/dxvk-d3d9.pc`) |
| DXVK source revision | `c3dd74be6baec53786d4e064a572185b70347a17` (also the source of `include/vulkan/include`) |
| SDL3 SDK (3.2.10, i386) | `/tmp/kisak-sdl3-sdk/root` (`usr/include`, `usr/lib/i386-linux-gnu`) |
| Vulkan headers | `<dxvk-source>/include/vulkan/include` or any Vulkan SDK |
| Toolchain | `g++ -m32` plus a 32-bit Vulkan loader/ICD and 32-bit SDL3 runtime |

The DXVK native build must include the SDL3 window system integration
(`-Dnative_sdl3=enabled` at configure time). Its pkg-config module is `dxvk-d3d9` and
`Cflags` already points at `<prefix>/include/dxvk`, so `<d3d9.h>` and
`<wsi/native_sdl3.h>` resolve with `-I$(pkg-config --cflags-only-I dxvk-d3d9)`.

Note on the SDK layout: the staged SDL3 SDK's `sdl3.pc` says `prefix=/usr`, so pkg-config
only turns its paths into staged locations with `PKG_CONFIG_SYSROOT_DIR`. That variable
would also rewrite the DXVK prefix, so this project never sets it. Use explicit `-I`/`-L`
(the manual command below) or `-DKDXK_SDL3_SDK_ROOT=/tmp/kisak-sdl3-sdk/root`.

## Build

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
errors. Runtime dependencies must still be present and match the i386 ABI.

CMake variant (pkg-config resolves `dxvk-d3d9`, SDL3 comes from the SDK root):

```sh
PKG_CONFIG_PATH=/tmp/kisak-dxvk-prefix/lib32/pkgconfig \
cmake -S tests/linux_renderer -B build/linux_renderer_smoke \
      -DCMAKE_CXX_FLAGS=-m32 -DCMAKE_EXE_LINKER_FLAGS=-m32 \
      -DKDXK_SDL3_SDK_ROOT=/tmp/kisak-sdl3-sdk/root \
      -DKDXK_VULKAN_INCLUDE_DIR=/tmp/kisak-dxvk-source/include/vulkan/include
cmake --build build/linux_renderer_smoke
```

The project refuses to configure unless the compiler produces 32-bit binaries, and it
checks at configure time that `d3d9.h`, `d3d9types.h`, `d3d9caps.h`, `windows.h` and
`wsi/native_sdl3.h` exist in the `dxvk-d3d9` include directories.

## Run

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

## What the run checks

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

## Files

| File | Purpose |
| --- | --- |
| `d3d9_sdl3_smoke.cpp` | the whole smoke test |
| `CMakeLists.txt` | standalone project (pkg-config `dxvk-d3d9`, SDL3 from pkg-config or `KDXK_SDL3_SDK_ROOT`) |
| `README.md` | this document |
