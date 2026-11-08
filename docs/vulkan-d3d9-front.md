# Vulkan D3D9 front

Opt-in, part of the [Legacy adapter](vulkan-legacy-adapter.md)'s build
(`-DKISAK_BUILD_VULKAN_LEGACY=ON`). `src/gfx_vk/d3d9` implements `IDirect3D9`,
`IDirect3DDevice9` and the resource interfaces on the adapter, so the
renderer's own Direct3D 9 calls, unchanged, draw through Vulkan. A check
program, `KisakCOD-vulkan-d3d9-check`, drives it through those interfaces and
checks what comes back. With `-DKISAK_BUILD_VULKAN_BACKEND=ON` the game's Linux
programs link it, and `r_backend vulkan` makes `R_HW_Direct3DCreate9` return
the front's `IDirect3D9` ([In the game](#in-the-game-r_backend-vulkan)).

## Why a front, not a second set of R_HW_* functions

The renderer reaches Direct3D 9 only through the `R_HW_*` functions in
`src/gfx_d3d/rhi`, and they take the D3D9 interfaces (`IDirect3DTexture9 *` and
so on), which the renderer also stores in its own structures. Implementing those
interfaces means one switch point (device creation) and no change to any call
site or structure: everything after `Direct3DCreate9` lands on the front through
the same vtables DXVK and `d3d9.dll` fill. The renderer cannot tell the
difference, which is what lets mp_crash be compared image by image with DXVK.

## Where it fits

```
renderer -> R_HW_* (src/gfx_d3d/rhi) -> IDirect3DDevice9 ...
                                        |-- d3d9.dll or DXVK (r_backend d3d9)
                                        `-- the D3D9 front (r_backend vulkan)
                                              -> Legacy adapter (src/gfx_vk/legacy)
                                              -> Vulkan core (src/gfx_vk)
```

The front compiles against the same D3D9 declarations as the renderer: the
Windows SDK on Windows, and on Linux and macOS the native D3D9 boundary
(`<linux/d3d9_types.h>` with `KISAK_LINUX_NATIVE_D3D9`, DXVK Native's headers;
see [linux-native-d3d9.md](linux-native-d3d9.md)). So the vtables, types and
calling conventions are the renderer's own. It links no D3D9 library. The
boundary renames DXVK's `HWND` and `POINT` while it reads DXVK's headers, so the
front declares its overrides with `D3D9Window` and `D3D9Point`
(`vk_d3d9.h`), which are the right type on each platform.

| File | What |
| --- | --- |
| `vk_d3d9.h` | `CreateDirect3D9(Host)`, the host callbacks, and `Counters` for checks. |
| `vk_d3d9_internal.h` | The classes: one per interface, each holding its adapter object. |
| `vk_d3d9_direct3d.cpp` | `IDirect3D9`: adapter, modes, caps, format checks, `CreateDevice`. |
| `vk_d3d9_device.cpp` | `IDirect3DDevice9`: creation and teardown, state, resources, draws, copies, `Reset`, `Present`. |
| `vk_d3d9_resources.cpp` | Reference counts, surfaces, textures, volumes, buffers, shaders, declarations, queries, the swap chain. |

## The host

D3D9 asks the OS about windows, modes and monitors; the front asks a `Host`
instead (`vk_d3d9.h`), which the program fills once:

* `windowSystem(window, out)`: the Vulkan window system for the window
  `CreateDevice` is given (on Linux and macOS the `HWND` is the SDL window).
* `modeCount`, `mode`, `currentMode`, `monitor`: display modes of the one
  adapter (reported as `X8R8G8B8`) and its monitor.
* `log`: one line per call.
* `validation`, `deviceIndex`, `pipelineCachePath`: how the core is created.
  Vsync follows `PresentationInterval` at `CreateDevice`.
* `coreDestroyed`: the core's final statistics at teardown (the check reads the
  validation counts here).

## Reference counts

Objects follow D3D9's two counts. The public count is the program's (`AddRef`,
`Release`). While the device has an object bound (render target, depth surface,
texture, stream, index buffer, shader, declaration) it holds it too, so an
object the program released but the device still draws with stays alive until
it is unbound, as in D3D9. Surfaces of a texture's levels and volumes of a
volume texture use their texture's counts (`GetContainer` returns it). The back
buffer and the implicit swap chain have counts of their own, starting at 0; while
the program holds one, it holds the device once, and they go with the device, as
in D3D9 and DXVK. The engine relies on this: before every reset and at shutdown
it asserts that releasing the swap chain returns 0 (`R_ReleaseAndSetNULL` in
`R_ReleaseForShutdownOrReset`). Until laptop run 4 found it, the front returned
the device's count there, so a resize, `vid_restart` or quit on Vulkan stopped
on that assert. The device itself goes once
its own count and its live objects are both zero; its last `Release` drops every
binding first. The check counts all of this (`references` scene, teardown).

## Threads

The renderer creates its device with `D3DCREATE_MULTITHREADED` and calls it
from more than one thread: during a map load the database thread creates and
fills textures while the render thread draws. So every method of every front
object, `AddRef` and `Release` included, holds one process-wide recursive
lock (`FrontLock`, `vk_d3d9_internal.h`), as DXVK holds its device lock. It is
one lock rather than one per device because the renderer has one device and a
`Release` can destroy the device whose method is running. The core and the
adapter stay single-threaded behind it. The host's callbacks run under the
lock too.

The check's `threads` scene does what the database thread does (a worker
creates, locks, fills, unlocks and releases 200 textures while the main thread
draws and presents), then draws the last one. Built with ThreadSanitizer it
reports 29 data races with the lock taken out (in the adapter's pass state and
the core's frame state) and none with it; hosted CI runs that build.

## What is implemented

Everything in `r_rhi.h`, and the getters D3D9 pairs with each setter:

* `IDirect3D9`: one adapter with a fixed identifier, modes from the host,
  `CheckDeviceFormat` from the adapter's format table (sRGB reads, volume render
  targets and cube depth refused), no multisampling, caps as DXVK 2.7.1 reports them
  for shader model 3 (zlib licence).
* Device: render and sampler state with getters, textures (2D, cube, volume)
  with their levels, surfaces and locks, render targets and depth surfaces,
  offscreen plain surfaces, static and dynamic buffers with D3D9's lock flags,
  shaders from bytecode (with `GetFunction`), vertex declarations,
  shader float constants, viewport and scissor, `Clear`, `DrawPrimitive`,
  `DrawIndexedPrimitive`, `DrawPrimitiveUP` (stream 0 unbound afterwards, as in
  D3D9), `StretchRect`, `UpdateTexture`, `GetRenderTargetData`,
  `GetFrontBufferData` (from the back buffer, at its size), event and occlusion
  queries, the implicit swap chain, `Reset` (new back buffer size, every state
  back to its default), `Present`, gamma ramps (kept, not applied: DXVK applies
  them only to fullscreen swapchains), `GetAvailableTextureMem` (device-local
  memory, below 4 GB).
* Defaults: the depth test is off after `CreateDevice` and `Reset`, because the
  front never makes an automatic depth-stencil surface (the renderer creates its
  own; `EnableAutoDepthStencil` is logged and ignored).

## What is not

These answer `D3DERR_INVALIDCALL` (or a harmless default where D3D9 has no
error), log one line per method and count in `DeviceCounters::unsupportedCalls`,
so a run can show that the renderer reached none of them:

* Fixed function: transforms, materials, lights, clip planes and status,
  texture-stage state, palettes, `SetFVF` with a non-zero FVF,
  `ProcessVertices`, software vertex processing, N-patches and patches.
* State blocks.
* `CreateAdditionalSwapChain` (only Radiant calls it), `UpdateSurface`,
  `ColorFill`, `DrawIndexedPrimitiveUP`, `SetCursorProperties`, surface `GetDC`.
* Instancing (`SetStreamSourceFreq` with a divider other than 1),
  `D3DUSAGE_AUTOGENMIPMAP`, shared resource handles, `SetAutoGenFilterType`.
* Integer and boolean shader constants: kept for the getters, but the Legacy
  shaders read them as zero, so a non-zero value counts as unsupported.
* `Present` with source or destination rects, a dirty region or another
  window: presented whole, and counted.

Other limits:

* No multisampling: `CreateDevice`, `Reset`, `CreateRenderTarget` and
  `CreateDepthStencilSurface` refuse it, and `CheckDeviceMultiSampleType`
  offers none, so the renderer keeps `r_aaSamples` at 1.
* The adapter identity and formats are static (the GPU is chosen at
  `CreateDevice`); the only identifier check in the renderer compares with
  NVPerfHUD's name.
* A change of `PresentationInterval` at `Reset` needs a new device; it is
  logged and kept.
* The adapter's own [known limits](vulkan-legacy-adapter.md#known-limits) apply.

## Building and running

On Linux the front needs the native D3D9 boundary for the declarations (headers
only; nothing of DXVK is linked):

```sh
cmake -S . -B build-vk -G Ninja -DKISAK_PLATFORM=linux -DKISAK_LINUX_ARCH=x86_64 \
  -DKISAK_BUILD_VULKAN_PROBE=ON -DKISAK_BUILD_VULKAN_SHADERS=ON -DKISAK_BUILD_VULKAN_LEGACY=ON \
  -DKISAK_BUILD_LINUX_NATIVE_D3D9=ON -DKISAK_DXVK_NATIVE_INCLUDE_DIR=<dxvk>/include/dxvk \
  -DSDL3_DIR=<sdl3-sdk>/lib/cmake/SDL3
cmake --build build-vk --target KisakCOD-vulkan-d3d9-check
KisakCOD-vulkan-d3d9-check [--validation off|auto|require] [--device N] [--scene NAME]
```

Without the boundary the front and its check are skipped with a message. On
Windows the standalone `scripts/vulkan_probe` build compiles them against the
SDK's `d3d9.h`.

The check opens a small window, creates the device through `IDirect3D9` with
a 128x128 back buffer and runs its scenes. Output is `key=value` lines: one
`d3d9_check.scene=<name> points=<n> mismatched=<n>` line per scene, the present
and unsupported-call counts, the live objects at teardown, the device's last
release, the validation counts, and `d3d9_check=passed` or
`d3d9_check=failed stage=<stage>` last. Exit status 0 is a pass, 1 a failure, 2
a usage error.

| Scene | Checks |
| --- | --- |
| `adapter` | Adapter count and identifier, modes from the host, caps, format and multisample checks. |
| `states` | Defaults after `CreateDevice`, render and sampler state round trips (vertex samplers included), constant ranges, refusals, one deliberate unsupported call. |
| `draw` | Static vertex and index buffers, a draw from memory (stream 0 unbound after it), a textured quad, a vertex shader constant, `GetTexture`. |
| `render_target` | A render-target texture's level with a depth surface (viewport follows the target), the getters, sampling it afterwards, `StretchRect` into the back buffer. |
| `shadow_map` | The sun shadow's path: a D24S8 texture made with `D3DUSAGE_DEPTHSTENCIL` as the depth target beside a R5G6B5 colour target, then sampled with linear filtering as a hardware shadow map (0 behind the stored depth, 1 in front). |
| `references` | Counts of textures and their levels, `GetContainer`, `QueryInterface`, a released texture that still draws while bound and goes when unbound, the back buffer's and swap chain's own counts (last `Release` returns 0) and the device reference they hold. |
| `threads` | A worker thread creating and filling textures while this thread draws and presents; the last one drawn and read back; nothing left alive. |
| `queries` | Support checks, an event query and an occlusion query finishing. |
| `reset` | `Reset` to 96x96 and back: the new size, default state, nothing bound, drawing at the new size. |
| `present` | `Present` through the device and the swap chain, `GetFrontBufferData`. |

`scripts/tests/test_vulkan_d3d9_front.py --binary <check>` runs it the way the
adapter's runner does: usage errors, an unknown scene, one scene alone (with no
unsupported call), and a full run in which every scene matches, the only
unsupported call is the deliberate one, frames are presented, no object
outlives its release, the device is destroyed at its last release and the
validation layer reports no errors.

The hosted `vulkan_window` job (Linux) stages DXVK Native's headers at the
x86_64 client's pin, builds the check in Debug and RelWithDebInfo and runs that
script on lavapipe with validation and synchronisation checks required. Its
Debug run also builds the check with ThreadSanitizer and runs the `threads`
scene, which fails on any data race. The
Windows `vulkan-probe` job builds it with MSVC, checks that it imports neither
`vulkan-1.dll` nor `d3d9.dll` and that it starts, and adds it to the
`KisakCOD-vulkan-probe-x64` artifact.

Results so far (2026-11-08, cloud sandbox, no GPU, lavapipe from Mesa 25.2.8):
all 10 scenes pass on x86_64 (Debug and RelWithDebInfo, validation with
synchronisation checks, no errors) and on i386 (Debug, no validation layer:
Ubuntu ships none for i386), with the one deliberate unsupported call.

## In the game: r_backend vulkan

`-DKISAK_BUILD_VULKAN_BACKEND=ON` (`scripts/vulkan_probe/vulkan_backend.cmake`,
with `KISAK_BUILD_VULKAN_LEGACY` and the native D3D9 boundary) links the front
into whichever of these programs the build configures:

| Program | How to choose Vulkan |
| --- | --- |
| `KisakCOD-device-probe` | `--backend vulkan [--validation off\|auto\|require]` |
| `KisakCOD-world-draw-probe` | `+set r_backend vulkan` |
| `KisakCOD-mp` | `+set r_backend vulkan` |

The probes are i386, as before; `KisakCOD-mp` is any client architecture. The
programs still link the system's D3D9 (DXVK Native) and use it by default.
`src/linux/client/vulkan/vulkan_backend.cpp` registers three dvars, all
archived and latched (a change applies at the next `vid_restart`):

* `r_backend`: `d3d9` (default: DXVK on Linux) or `vulkan`.
* `r_vkValidation`: `off` (default), `auto` (when installed) or `require`.
* `r_vkDevice`: -1 (default) lets the core pick a GPU; N picks the Nth device
  the Vulkan driver lists.

With `r_backend vulkan`, `R_HW_Direct3DCreate9`
(`src/gfx_d3d/rhi/r_rhi_d3d9_calls.cpp`, built with `KISAK_VULKAN_BACKEND`)
returns the front and DXVK is never called. There is no fallback: if the Vulkan
device cannot be made, `R_CreateDevice` fails as it would with DXVK. The host
is SDL3, as for DXVK's SDL3 WSI: the `HWND` the Win32 window layer makes is the
`SDL_Window`, the `HMONITOR` is the primary display's `SDL_DisplayID`, the
display modes are SDL's fullscreen modes (or the desktop mode where SDL lists
none, as with its offscreen driver), and the front's log lines go to the
console as `Vulkan: ...`. When the device goes, the front logs where the CPU
time went (`vulkan d3d9: device stats: ...`: shaders created and the time
translating them, pipelines and the time building them, texture uploads with
their size and time), for comparing loads and first frames with DXVK. No
pipeline cache file is written yet.

### On Windows

On Windows the same option links the front into `KisakCOD-mp` only
(`scripts/vulkan_probe/vulkan_backend_windows.cmake`, included from the root
`CMakeLists.txt` before `scripts/mp`). The one option is enough: it builds the
Vulkan core, the shader translator, the Legacy adapter and the front as
libraries only (`KISAK_VULKAN_LIBRARIES_ONLY`: no SDL3, none of the tools), with
the game's static C runtime. It needs MSVC (Win32, x64 or ARM64; MinGW refuses
it) and the Vulkan headers 1.3 or newer:

```
cmake -S . -B build -A Win32 -DDXSDK_DIR=<dxsdk> -DKISAK_BUILD_VULKAN_BACKEND=ON ^
  -DKISAK_VULKAN_INCLUDE_DIR=<Vulkan-Headers>/include
```

With the Vulkan SDK installed, `VULKAN_SDK` is found and the include directory
can be left out. dxbc-spirv is fetched at configure time, as for the tools.

The host is `src/win32/vulkan/win_vulkan_backend.cpp`, with plain Win32 and the
same three dvars:

* The `HWND` is the game's own window. The surface comes from
  `vkCreateWin32SurfaceKHR`, and the size in pixels from `GetClientRect` (0 x 0
  while minimised).
* `vulkan-1.dll` is opened at run time from the system directory
  (`LoadLibraryExW` with `LOAD_LIBRARY_SEARCH_SYSTEM32`), never imported. The
  game still starts on a machine without Vulkan; `r_backend vulkan` then fails
  at `Direct3DCreate9`, as a missing D3D9 would.
* Display modes are the primary display's 32-bit modes (`EnumDisplaySettingsW`),
  and the monitor is the primary one.

The front never changes the display mode. In fullscreen the window covers the
mode's size at the desktop's resolution, so use windowed mode or the desktop's
own resolution for now. The Windows workflow builds it for Win32 Debug and for
x64 (whose artifact is the PC's), checks the configure log says `Vulkan back
end: linked into KisakCOD-mp`, and checks the Win32 executable does not import
`vulkan-1.dll`. Hosted runners have no GPU, so the game has not drawn through
it on Windows yet.

The device probe with `--backend vulkan` runs `r_init.cpp`'s device start
unchanged (`R_PreCreateWindow`, `R_CreateWindow`, `R_CreateDevice`), clears,
reads back and presents, and also requires no unsupported call, the device
destroyed at its last release and no validation error. The hosted
`vulkan_engine` job builds the device probe, the world draw probe and
`KisakCOD-mp` for i386 with the back end, runs the device probe that way on
i386 lavapipe through SDL's offscreen driver, and runs the front's check there
too. In the sandbox (2026-11-08) the probe passed: shader model 3 path picked,
640x480 back buffer read back with no pixel off, 3 frames presented, nothing
unsupported, the device destroyed at release.

### Comparing with DXVK

The world draw probe writes each view's settled frame beside its report with
`KISAK_WORLD_DRAW_IMAGES=1` (`view-A.ppm`, `view-B.ppm`). Run it twice on the
same machine into two private output directories, once with
`+set r_backend d3d9` and once with `+set r_backend vulkan` (the rest of the
command as in [linux-world-draw-probe.md](linux-world-draw-probe.md)), then:

```sh
python3 scripts/tools/vulkan/compare_frames.py dxvk/view-A.ppm vulkan/view-A.ppm --diff diff-A.ppm
```

It prints the mean and largest channel difference, the share of pixels off by
more than 8 and by more than 32, and the PSNR, and writes the difference
scaled up so that what differs shows bright. The images stay with the reports
on that machine.

Laptop run 1 (2026-11-08, Intel iGPU, ANV, i386 RelWithDebInfo, 289ae039):
the front check, the device probe with `--backend vulkan` and mp_crash through
the world draw probe all pass, and two Vulkan runs give identical frames, but
the lit world drew brighter than through DXVK (view A PSNR 17.0, view B 20.7).
Run 2 switched features off one at a time: with `r_specular 0` the two back
ends match (PSNR 57.9 and 59.8, no pixel more than 2 apart). The cause was the
translator computing `_pp` instructions in 16-bit floats, where the specular
terms overflow ([vulkan-legacy-adapter.md](vulkan-legacy-adapter.md)); it now
computes them at 32 bits, as DXVK does. Run 3 (79186f8e) confirmed it: view A
mean_abs 0.001, PSNR 66.7 (about 120 pixels in one small spot differ, 9 of
them by more than 8 levels), view B max 1, PSNR 100; view B's Vulkan digest is
the one DXVK 3.1.1 (dxbc-spirv too) gave in the shader spike. Vulkan took
16.8 s against DXVK's 6.2 s for the whole probe. Reports:
`renderer/vulkan-backend-laptop-run-{1,2,3}.md` in the project files.

## Next

`KisakCOD-mp` played with `r_backend vulkan` on the laptop (laptop run 4); the
PC comparison through the Windows host; and the load-time gap. Laptop run 4
measured it at +4.5 s warm, mostly while zones load (shader translation and
uploads), and +9.9 s cold, mostly pipelines built at the first draw.
