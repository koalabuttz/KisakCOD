# D3D9 device probe (opt-in)

`KisakCOD-device-probe` creates the Direct3D 9 device on native i386 Linux
through the engine's own `src/gfx_d3d/r_init.cpp`, compiled unmodified through
the [native D3D9 boundary](linux-native-d3d9.md), with DXVK Native 2.7.1 as the
D3D9 implementation and an SDL3 window. It is the device step of the static map
view milestone ([order of work](linux-world-frontend.md#order-of-work-toward-one-static-map-view)).
It reads no game data, writes nothing, and keeps no image: the one read-back is
compared in memory.

## What runs

The part of `R_InitGraphicsApi` that ends with a device, in the engine's order,
then checks on what it produced (`src/linux/tools/device_probe.cpp`):

| Stage | Code | Fails closed when |
| --- | --- | --- |
| `environment` | the probe | `DXVK_WSI_DRIVER` is set to anything but `SDL3` (an unset variable is set to `SDL3`; any other WSI would misread every `HWND`) |
| engine runtime | `Sys_InitMainThread`, `SL_Init`, `Dvar_Init`, `sys_smp_allowed` as `Com_InitDvars` registers it, `R_RegisterDvars`; then `r_fullscreen` off and `r_customMode` set to the requested size, latched values made current | an engine assertion or `Com_Error` |
| `direct3d` | `R_PreCreateWindow`: `Direct3DCreate9`, `R_ChooseAdapter` (monitor selection), `R_StoreDirect3DCaps` (`R_PickRenderer`, shadow-map formats), `R_EnumDisplayModes` | no D3D9 interface; no display mode of at least 640x480 (the engine's own check) |
| `adapter`, `renderer` | `GetAdapterIdentifier`, `GetDeviceCaps` | `R_PickRenderer` did not choose the shader model 3 path. The game's zones carry shader model 3 shaders only (checked on mp_crash by the [front-end probe](linux-world-frontend.md#a-real-map-the-offline-front-end-probe)) |
| `window` | `R_SetWndParms`, `R_CreateWindow` (`CreateWindowExA`) | the engine did not take the requested windowed size, or the window could not be created |
| `device` | `R_CreateDevice`: present parameters, depth-stencil format, `IDirect3D9::CreateDevice` | no device, or `TestCooperativeLevel` is not `D3D_OK` |
| `readback` | `Clear` to `0xFF336699`, `GetRenderTargetData` into a system-memory surface, `LockRect` | the back buffer is not the window size or not 32-bit RGB, or any pixel's RGB differs |
| `present` | `Clear`, `BeginScene`, `EndScene`, `Present` per frame, pumping SDL events | any call fails |
| `shutdown` | device, window, Direct3D object released in `R_ShutdownDirect3D`'s order | a reference or a window outlives the release |

`R_InitHardware`'s later steps (zones, render targets, Bink, initial state,
scene, systems) are not run: they need assets or units that are not linked yet.
`R_ShutdownDirect3D` is not called for the same reason (it also releases what
those steps make).

Output is `key=value` lines, interleaved with the engine's own messages. The last
line is `device_probe=passed renderer=shader_model_3 frames=N` or
`device_probe=failed stage=<stage> reason=<text>`. Exit status: 0 passed, 1 a
stage failed, 2 a usage error. An engine assertion prints
`device_probe=failed stage=assert reason=<file>:<line>: <message>` and aborts.

## The Win32 window layer

`r_init.cpp` owns the window on Windows. It calls `CreateWindowExA`,
`MonitorFromPoint`, `EnumDisplayMonitors`, `GetMonitorInfoA`, `GetSystemMetrics`
and friends, and stores `IDirect3D9::GetAdapterMonitor`'s `HMONITOR`. Two pieces
let it compile and run unmodified:

- `src/linux/win32_window.h` declares those calls with the engine's stdcall
  convention and the Windows layouts (`MONITORINFO` is 40 bytes, `rcWork` at 20),
  plus `Sleep` for `rb_sky.cpp`. The boundary header includes it last, so only
  units compiled with `KISAK_LINUX_NATIVE_D3D9` see it. `HMONITOR` and `HDC` are
  in `compat.h`; how they meet DXVK's is in
  [linux-native-d3d9.md](linux-native-d3d9.md#design).
- `src/linux/client/window/win32_window_sdl3.cpp` implements them over SDL3,
  with the handle encoding DXVK's SDL3 WSI uses, so the handles the engine gets
  from DXVK and from this layer are the same values:

| Win32 | SDL3 |
| --- | --- |
| `HWND` | `SDL_Window *` (created with `SDL_WINDOW_VULKAN`; `WS_VISIBLE` clear means hidden, `WS_POPUP` borderless, `WS_EX_TOPMOST` always on top) |
| `HMONITOR` | the `SDL_DisplayID`, as DXVK's `GetAdapterMonitor` returns it |
| `GetMonitorInfoA` | display bounds and usable bounds; the primary flag from `SDL_GetPrimaryDisplay` |
| `GetSystemMetrics(SM_CXSCREEN/SM_CYSCREEN)` | the primary display's current mode |

It fails closed instead of guessing: a parent window, menu or creation
parameter, a non-positive size, a device context or clip rectangle passed to
`EnumDisplayMonitors`, an unknown display, a short `cbSize`, and a named module
are refused; `ShellExecuteA` refuses with `SE_ERR_ACCESSDENIED` (5);
`MessageBoxA` writes to stderr and returns `IDOK`. `IsWindow`, `ShowWindow` and
`DestroyWindow` only accept windows this layer created. SDL video is started on
first use and kept for the life of the process.

`KisakCOD-win32-window-test` (`tests/linux_window/win32_window_sdl3_test.cpp`)
checks the layer on SDL's `dummy` video driver, with no D3D9 library: style
flags, that nothing starts SDL video before a call needs it, monitor lookup and
enumeration (including an engine-shaped stdcall callback), and every refusal
above. The dummy driver cannot create a Vulkan window, so window creation is
checked to fail cleanly there; creating a real window is the probe's job.

## How it links

`scripts/platform/linux/device_probe.cmake` (option `KISAK_BUILD_DEVICE_PROBE`,
off by default) needs `KISAK_BUILD_LINUX_NATIVE_D3D9`, `KISAK_DXVK_NATIVE_LIBRARY`
and SDL3 3.2.10 (`find_package(SDL3 CONFIG)`). Production units are compiled
unmodified with the client definitions the world front-end probe uses:

- through the boundary: `r_init.cpp`, `r_rendertarget.cpp`
  (`R_GetDepthStencilFormat`) and `r_material.cpp` (the technique override
  `R_RegisterDvars` sets);
- with the type-only stand-in: the renderer dvars (`r_dvars.cpp`, `r_sky.cpp`,
  `r_warn.cpp`), the dvar system and the script string list behind string dvars,
  and the Linux thread, critical-section and sleep services.

The link uses `--gc-sections`. `src/linux/tools/device_probe_boundary.cpp`
defines what no linked unit does, in three kinds: host services (console output
to stdout, `Com_Error` thrown so the probe names the stage, assertions that print
the failure line and abort, and a verbatim copy of `common.cpp`'s `_copyDWord`,
because `common.cpp` is the host this replaces); dvar storage whose owner is not
linked (`r_fullscreen`, `vid_xpos`, `vid_ypos`, `sv_cheats`, `com_statmon`,
`sys_smp_allowed`, null until registered, as in their owners); and
`Win_LocalizeRef`, reachable only after `Sys_DirectXFatalError`'s own assertion,
which aborts with its name. The probe's own code builds with
`-Wall -Wextra -Werror`.

## Building and running

DXVK Native must be the i386 2.7.1 build with `-Dnative_sdl3=enabled`
([tests/linux_renderer/README.md](../tests/linux_renderer/README.md)); SDL3 is
3.2.10, i386, with a video driver that can make Vulkan windows (X11 or Wayland).
A 32-bit Vulkan loader and driver are needed at run time. Build outside the
checkout:

```sh
cmake -S . -B ~/kisak-build/device-rel -G Ninja -DKISAK_PLATFORM=linux \
  -DCMAKE_BUILD_TYPE=RelWithDebInfo -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++ \
  -DCMAKE_RUNTIME_OUTPUT_DIRECTORY=$HOME/kisak-build/device-rel/bin \
  -DKISAK_BUILD_LINUX_NATIVE_D3D9=ON -DKISAK_DXVK_NATIVE_INCLUDE_DIR=<dxvk-prefix>/include/dxvk \
  -DKISAK_DXVK_NATIVE_LIBRARY=<dxvk-prefix>/lib32/libdxvk_d3d9.so \
  -DKISAK_BUILD_DEVICE_PROBE=ON -DSDL3_DIR=<sdl3-sdk>/lib/cmake/SDL3
cmake --build ~/kisak-build/device-rel --target KisakCOD-device-probe KisakCOD-win32-window-test

LD_LIBRARY_PATH=<dxvk-prefix>/lib32:<sdl3-sdk>/lib \
  ~/kisak-build/device-rel/bin/KisakCOD-device-probe --width 640 --height 480 --frames 60
```

Options: `--width` and `--height` (64 to 8192, default 640x480; the window must
fit the monitor) and `--frames` (1 to 600, default 3).

## CI

The `device_probe` job (Debug and RelWithDebInfo) builds the pinned SDL3 (no X11,
no Wayland), fetches the pinned DXVK headers, and builds both targets against
`tests/linux_device_probe/d3d9_unavailable.cpp`, a stand-in `libd3d9_unavailable.so`
whose `Direct3DCreate9` returns null. It checks both are ELF32 i386, that the
probe needs SDL3 and the D3D9 library and the window test does not need the D3D9
library, runs the window test on SDL's dummy driver, and runs the probe, which
must stop at `stage=direct3d` with exit 1 after the engine runtime came up, and
exit 2 on a bad argument. No DXVK library, Vulkan, GPU, display or game data is
involved. A pass there means the probe builds and fails closed; it says nothing
about a device.

## Evidence

Local only, in the cloud sandbox, 2026-11-02, on the branch that adds the probe
(based on 91116b8). Not CI evidence, and not hardware evidence: there is no GPU.

- Ubuntu 24.04, clang 18.1.3. DXVK Native 2.7.1 built for i386 from c3dd74be
  (d3d9 only, `-Dnative_sdl3=enabled`); SDL 3.2.10 i386 built with X11; Xvfb
  21.1.12 at 1920x1080x24; Mesa 25.2.8 lavapipe (i386), a software Vulkan driver.
- Debug and RelWithDebInfo: `device_probe=passed renderer=shader_model_3
  frames=60`, exit 0, with the same values in both. Pixel
  and vertex shader 3.0, the engine logged "Using Shader model 3.0 code path";
  maximum texture 16384, 8 texture units, 1 display mode, anisotropy 16, 6 clip
  planes, hardware shadow map with D24S8 (`0x4B`). Window 640x480 at (3, 22),
  60 Hz; device created, cooperative level `D3D_OK`; read-back 640x480
  A8R8G8B8 with 0 mismatched pixels; 60 frames presented in 0.16 to 0.19 s;
  shutdown with no references and no windows left.
- DXVK reports lavapipe as "AMD Radeon RX 6700 XT" (0x1002/0x73DF): it gives any
  vendor other than AMD, NVIDIA and Intel that identity. The adapter line is not
  evidence of the hardware.
- The stand-in library run stops at `stage=direct3d` with exit 1, as in CI.

## Limits

- SDL's `offscreen` driver lists no fullscreen modes, so `R_EnumDisplayModes`
  reports "No valid resolutions of 640 x 480 or above found" and the engine
  stops in its fatal-error path (`Sys_DirectXFatalError`'s assertion,
  `r_init.cpp:365`). A headless run needs a display server such as Xvfb.
- Only the shader model 3 path is accepted. Only windowed mode, the renderer
  dvars' defaults (no multisampling) and the first adapter are exercised.
- Device loss, reset, resize and fullscreen are not exercised. No resource is
  created and nothing is drawn but clears.
- On hardware it has not run yet; that evidence belongs on a developer machine
  and is recorded here when it exists.
