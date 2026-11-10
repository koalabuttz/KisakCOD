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
* `windowOrigin`: where the window's client area starts on its monitor. A
  windowed screenshot asks `GetFrontBufferData` for a surface the size of the
  display, as D3D9 does; the front puts the last frame shown there at that
  place.
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

The lock is taken thousands of times a frame by the render thread, and the
engine's main thread asks for it too: while the GPU is behind, `R_SyncGpu`
polls the GPU sync fence every 20 microseconds, for milliseconds of each
frame. Two things keep that from costing the render thread:

* A thread that finds the lock taken tries again for a short while without
  sleeping (2000 tries, tens of microseconds), as DXVK's device lock does, and
  only then sleeps on the mutex. Nearly every call holds the lock for a
  microsecond or less; a waiter that sleeps at once makes the holder pay a
  system call to wake it, and then often finds the lock taken again.
  `KISAK_VK_FRONT_SPIN=0` makes every waiter sleep at once, to compare. The PS
  Vita build keeps the plain recursive mutex.
* `IDirect3DQuery9::GetData` asks the GPU how far it has got before it takes
  the lock (`BackendPollProgress`, the core's `RefreshProgress`,
  [vulkan-core.md](vulkan-core.md)), which is the part that takes a system
  call. Under the lock the poll is then a comparison.

The check's `bench` scene measures this: the time one thread spends on the
3072 calls of a 768-draw frame while another polls a fence every 20
microseconds. In the cloud sandbox (4 cores, lavapipe, RelWithDebInfo) that
was 6.9 to 8.8 ms a frame before the GPU feed and the two changes (eight
runs) and 3.2 to 5.9 ms after (sixteen). With the feed off it was 3.7 to
4.6 ms, and with `KISAK_VK_FRONT_SPIN=0` 9.2 to 11.3 ms, so there the spin is
what pays. Those are local numbers from a virtual machine, not a GPU's.

Behind the lock, the front creates the core with its record thread on
(`CoreDesc::recordThread`, see [vulkan-core.md](vulkan-core.md)):
the core's own thread makes the `vkCmd*` calls, submissions and presents, as
DXVK's command stream thread does, so the render thread no longer waits on the
driver while it records. `KISAK_VK_RECORD_THREAD=0` turns it off. The device's
teardown log line `record thread:` gives the commands and chunks per frame and
the time the render thread waited for that thread.

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
  `GetFrontBufferData` (the last frame shown, kept apart from the back buffer),
  event and occlusion
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
and unsupported-call counts, the live objects at teardown, the window going
before the device as at the game's quit (`d3d9_check.window_detached=1
present_after=skipped`), the device's last release, the validation counts, and `d3d9_check=passed` or
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
| `queries` | Support checks, an event query and an occlusion query finishing, and an event issued right after `Present` (the engine's swap fence) finishing without a flush while the next frame records; polled with `D3DGETDATA_FLUSH` while its frame is unfinished, it submits the next frame's work once, and the frame goes on with its state and contents. |
| `frame_loop` | The engine's frame for 48 frames: draw, poll the swap fence with `D3DGETDATA_FLUSH` until the frame before has finished, issue the GPU sync fence, `Present`, issue the swap fence, while another thread polls the sync fence as the main thread does. At most three submissions a frame, at most one swap fence flush, and the last frame, drawn on both sides of its wait, comes out whole. |
| `gpu_feed` | A frame of 600 draws, each filling a cell of its own, with a scissor rectangle set once that cuts the last column. The adapter submits such a frame in parts while it is drawn ([adapter](vulkan-legacy-adapter.md)): every draw must show whichever part it fell in, the cells no draw touches must keep the clear colour, and the scissor must hold in every part. Prints the number of parts. |
| `bench` | Only when named (`--scene bench`), never in a full run: 120 frames of 768 draws of four D3D9 calls each, ended as `frame_loop`'s are, while another thread polls the sync fence every 20 microseconds. Prints the time the drawing thread spent on its draw calls per frame and the polls made meanwhile; passes when its frames were presented. |
| `reset` | `Reset` to 96x96 and back: the new size, default state, nothing bound, drawing at the new size. |
| `present` | `Present` through the device and the swap chain, then `GetFrontBufferData` halfway through the next frame: it must give the frame last shown. |

`scripts/tests/test_vulkan_d3d9_front.py --binary <check>` runs it the way the
adapter's runner does: usage errors, an unknown scene, one scene alone (with no
unsupported call), the `bench` scene, and a full run in which every scene
matches, the only unsupported call is the deliberate one, frames are presented,
no object outlives its release, the device is destroyed at its last release and
the validation layer reports no errors. It reads the GPU feed's setting from
the device's log line and holds the `gpu_feed` scene's parts to it: none with
the feed off, one for every draw with `KISAK_VK_GPU_FEED=always`, else one to
four.

The hosted `vulkan_window` job (Linux) stages DXVK Native's headers at the
x86_64 client's pin, builds the check in Debug and RelWithDebInfo and runs that
script on lavapipe with validation and synchronisation checks required. Its
Debug run repeats the script with the GPU feed off and with
`KISAK_VK_GPU_FEED=always` (and the adapter's check with the latter), and
builds the check with ThreadSanitizer and runs the `threads`, `frame_loop`
(also with `KISAK_VK_GPU_FEED=always`), `gpu_feed` and `bench` scenes, which
fail on any data race in our code (llvmpipe's rasterizer threads are off
there, [vulkan-core.md](vulkan-core.md)). The
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
`src/linux/client/vulkan/vulkan_backend.cpp` registers four dvars, all
archived and latched (a change applies at the next `vid_restart`):

* `r_backend`: `d3d9` (default: DXVK on Linux) or `vulkan`.
* `r_vkValidation`: `off` (default), `auto` (when installed) or `require`.
* `r_vkDevice`: -1 (default) lets the core pick a GPU; N picks the Nth device
  the Vulkan driver lists.
* `r_vkShaderCache`: 1 (default) keeps two caches in `<fs_homepath>/vulkan`
  between runs: the translated shaders in `shaders.bin` (the Legacy adapter's
  shader cache), and the Vulkan pipeline cache in `pipelines.bin` (the core's,
  ignored when another device or driver wrote it). 0 translates every shader
  and builds every pipeline from scratch at every start.

With `r_backend vulkan`, `R_HW_Direct3DCreate9`
(`src/gfx_d3d/rhi/r_rhi_d3d9_calls.cpp`, built with `KISAK_VULKAN_BACKEND`)
returns the front and DXVK is never called. There is no fallback: if the Vulkan
device cannot be made, `R_CreateDevice` fails as it would with DXVK. The host
is SDL3, as for DXVK's SDL3 WSI: the `HWND` the Win32 window layer makes is the
`SDL_Window`, the `HMONITOR` is the primary display's `SDL_DisplayID`, the
display modes are SDL's fullscreen modes (or the desktop mode where SDL lists
none, as with its offscreen driver), and the front's log lines go to the
console as `Vulkan: ...`. When the device goes, the front logs where the CPU
time went, for comparing loads and first frames with DXVK. The
`vulkan d3d9: device stats: ...` line gives the shaders created and how many
of them were translated (the rest came from the shader cache) with the time
it took, the pipelines built with their time, and the texture uploads with
their size and time. The core logs `pipeline cache: N bytes from ...` when it
starts from the pipeline cache file, and `pipeline cache: N bytes to ...`
when it writes the file back at teardown.

At quit `R_ShutdownDirect3D` destroys the game's window before it releases the
device, and a Vulkan surface must not outlive its window. So the window layer
tells the host first (`SetRendererWindowDestroying` in
`src/linux/client/window/win32_window_sdl3.h`, called from `DestroyWindow`
before `SDL_DestroyWindow`), and the host calls `front::WindowDestroying`: each
device made for that window ends an open frame unshown, waits for the GPU and
destroys its swapchain and surface (`vulkan: window detached: ...`). The device
stays usable until its release, and its presents are skipped. SDL also unloads
the Vulkan loader with its last Vulkan window, which used to leave the device's
teardown calling into an unloaded library (a crash at quit, after the `device
stats` line); the host holds one more loader reference
(`SDL_Vulkan_LoadLibrary`) for the rest of the process.

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
* `WM_DESTROY` (`src/win32/win_wndproc.cpp`) calls
  `Kisak_VulkanWindowDestroying`, which detaches the device's surface from the
  window as on Linux. `vulkan-1.dll` is never unloaded.

The front never changes the display mode. In fullscreen the window covers the
mode's size at the desktop's resolution, so use windowed mode or the desktop's
own resolution for now. The Windows workflow builds it for Win32 Debug and for
x64 (whose artifact is the PC's), checks the configure log says `Vulkan back
end: linked into KisakCOD-mp`, and checks the Win32 executable does not import
`vulkan-1.dll`. Hosted runners have no GPU, so CI never draws the game through
it. On a Windows PC with the game files (RTX 2080 Super, Win32 Debug builds
from CI, 2026-11-08; not CI evidence) the game draws mp_crash on Vulkan and
quits cleanly. From one indoor view, Vulkan and the real `d3d9.dll` agree at
43.9 dB in windowed desktop captures and 37.2 dB in exclusive-fullscreen
screenshots; repeating the same comparison gives 37.6 to 43.9 dB, so that
is the noise of the view.
With the shader cache, a warm start reaches the map in 15 s against 62 s
cold; the pipeline cache saves about 0.3 s more on that driver. Reports:
`renderer/vulkan-backend-pc-run-{1,2,5}.md` in the project files.

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

## The game on the laptop

`KisakCOD-mp` plays mp_crash with `r_backend vulkan` on the laptop (Intel
iGPU, ANV, x86_64 RelWithDebInfo, runs 4 to 7, 2026-11-08 and 09; not CI
evidence):

* Load time: run 4 measured Vulkan at +4.5 s warm, mostly while zones load
  (shader translation and uploads), and +9.9 s cold, mostly pipelines built
  at the first draw. The shader and pipeline caches (`r_vkShaderCache`) came
  after that run.
* Frame rate at a fixed view: 116 to 125 fps on battery against 151 to 159
  through DXVK (runs 5 and 6), while the back end made every driver call on
  the render thread and handed the GPU each frame whole. With the record
  thread, the GPU feed and the cheaper fence polls ([Threads](#threads),
  [adapter](vulkan-legacy-adapter.md#how-a-draw-works)), run 9 measured 140.9
  fps against DXVK's 151.3, on mains in power-saver mode, where the code
  before them gave 117.6 (run 8).
* Picture: models drew dark where the profile had
  `r_altModelLightingUpdate 0`, because the adapter dropped a texture's
  contents when it was locked again; fixed, and run 7 matches DXVK at 40.1 dB
  there. With `r_altModelLightingUpdate 1` (the default) models are still 2.7
  to 6.7 luma darker than through DXVK (33.6 dB); not explained yet.

Reports: `renderer/vulkan-backend-laptop-run-{4,5,6,7,8,9}.md` in the project
files.

## Next

The rest of the frame rate gap to DXVK (how often to feed the GPU, then the
render thread's time per draw), the model lighting difference above,
occlusion queries (the adapter reports every sample as passed), and a long
frame about every two minutes.
