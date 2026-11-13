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
* `fullscreen(window, mode)`: D3D9 switches the monitor to a fullscreen
  device's mode and covers it with the device window. For a device made or
  reset with `Windowed` false the front asks the host for the back buffer's
  size at `FullScreen_RefreshRateInHz` (`vulkan d3d9: fullscreen at WxH, R
  Hz`), and for the desktop's mode back (a null mode) when the device is reset
  to windowed or released. Null: a fullscreen device stays a window where the
  program put it.
* `log`: one line per call.
* `validation`, `deviceIndex`, `pipelineCachePath`: how the core is created.
  Vsync follows `PresentationInterval` at `CreateDevice`.
* `coreDestroyed`: the core's final statistics at teardown (the check reads the
  validation counts here).
* `tuning`: settings the program may change while the device exists
  (`Tuning`: the GPU feed on, off or after every draw, its draws to a part,
  the idle rule's count, and whether a draw takes up the state the draw
  before it left, see the [adapter](vulkan-legacy-adapter.md)).
  The front asks at every `Present` and takes up what changed, so a change
  shows in the next frame. In the game these are dvars.

`LogDeviceStats(reset)` logs every live device's frame statistics through the
host, from any thread: the lines a device logs when it is destroyed, counted
since the device was made or since the last call that started them again.

* `vulkan d3d9: frame stats:` gives the frames presented, the seconds they
  took and the frames a second; per frame the draws, passes, descriptor sets
  written and reused, constant data, buffer renames and submissions; the
  flushes by kind (at a swap fence, to feed the GPU); and the GPU frame time,
  which runs from a frame's first command to its last.
* `vulkan d3d9: GPU time:` splits that time: the milliseconds per frame the
  GPU spent in the frame's passes, the milliseconds between them (the GPU
  waiting for the frame's next part), and the milliseconds before the frame
  with nothing of the last frame or this one to do. A GPU that is the limit
  shows next to nothing in the last two. It also counts the parts the idle
  rule sent early. The line needs GPU timestamps and is left out without
  them. With `KISAK_VK_PASS_TIMESTAMPS=0` the passes are not timed (each
  one's closing timestamp waits for its work, which may cost a GPU that
  drains between passes): the line then gives the frame's time from first
  command to last and the time before it, says `passes not timed`, and so do
  the slow frame lines.
* `vulkan d3d9: slow frame N:` (up to three lines, the longest first) names
  the frames of the stretch that took longest, for the one long frame among
  thousands that an average hides. A frame here runs from the start of one
  `Present` to the start of the next, so it holds the engine's work, the
  device's, and every wait in between. Each line gives the time, the frame's
  number in the stretch and the second it ended at, and what the device's
  counters say happened in it: draws, passes, submissions, buffer renames and
  swapchains built; the pipelines built (how many of them the pipeline
  cache held, and how many pixel shaders were built for them with their own
  milliseconds: the rest of the pipelines' time is the driver's), the
  pipelines a draw waited for a worker to finish building ahead (see
  "Pipelines built ahead" in
  [vulkan-legacy-adapter.md](vulkan-legacy-adapter.md)), the shaders made,
  the texture levels uploaded and the memory
  blocks allocated, each with its milliseconds; and the milliseconds the
  calling thread waited for a swapchain image, for the frames in flight (with
  two frames in flight, a `Present` waits for the frame before the one it
  shows), for the GPU otherwise (a texture read back, a flush that waits) and
  for the record thread. Then the front lock's sleeps in the frame, on any
  thread: waits that spinning did not end, with the milliseconds from the end
  of the spin to holding the lock (none on the PS Vita, whose lock is a plain
  mutex). A thread that waits there long is held up by another thread inside
  the front. The parts are measured where they happen and can
  overlap (an upload that needed a memory block counts in both). What they
  leave of the frame's time is the engine's own work and the driver's inside
  ordinary calls. The line ends with the GPU's times for that frame (its
  length, the part in passes, the rest between them, and the time without work
  before it) once they have been read, which is a few frames after the frame.
  On lavapipe a submission runs inside `vkQueueSubmit`, on the record thread,
  so waiting for the GPU shows there as waiting for the record thread.
* `vulkan d3d9: record thread:` is as described under Threads.
* `vulkan d3d9: occlusion queries:` says whether they count, the Vulkan
  queries and pools they took, and what they answered in the stretch: the
  results worked out, how many of them had no sample, their average, and the
  newest. The engine asks one question with them, how much of the sun's
  sprite shows ([the adapter's guide](vulkan-legacy-adapter.md)). So with the
  sun's place on screen in the open the results are that sprite's samples,
  and with it behind a wall every result has no sample: `r_vkStats` twice, a
  few seconds apart, tells the two apart without a picture.
* `vulkan d3d9: modern:` appears once the Modern path has run
  (`r_visuals modern`, [vulkan-modern.md](vulkan-modern.md)): the scenes it
  anti-aliased in the stretch, how many of them had no history to use (the
  first, a camera cut, a new size), and the GPU time of its pass.
* `vulkan d3d9: gamma:` appears when frames of the stretch went to the window
  through the fullscreen gamma ramp (`r_gamma` with `r_fullscreen 1`): how
  many, and the GPU time of that pass.

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
* The lock is one word (free, held, held with sleepers) and its owner, both
  atomics: taking and dropping it without a waiter is one atomic operation
  each, inline in the method, with no call into the C library. Only a waiter
  that runs out of tries touches a mutex and condition variable to sleep on.
  The object needs no constructor that runs, so a call from a static
  constructor or destructor still finds it.
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
  targets and cube depth refused), multisampling at 2, 4 and 8 samples with one
  quality level (see below), caps as DXVK 2.7.1 reports them for shader model 3
  (zlib licence).
* Device: render and sampler state with getters, textures (2D, cube, volume)
  with their levels, surfaces and locks, render targets and depth surfaces,
  offscreen plain surfaces, static and dynamic buffers with D3D9's lock flags,
  shaders from bytecode (with `GetFunction`), vertex declarations,
  shader float constants, viewport and scissor, `Clear`, `DrawPrimitive`,
  `DrawIndexedPrimitive`, `DrawPrimitiveUP` (stream 0 unbound afterwards, as in
  D3D9), `StretchRect`, `UpdateTexture` (only what is dirty, as in D3D9:
  `AddDirtyRect`, `AddDirtyBox` and locks without `D3DLOCK_NO_DIRTY_UPDATE`
  mark it), `GetRenderTargetData`,
  `GetFrontBufferData` (the last frame shown, kept apart from the back buffer),
  event and occlusion
  queries, the implicit swap chain, `Reset` (new back buffer size, every state
  back to its default), `Present`, gamma ramps (applied as D3D9 and DXVK
  apply them: for a fullscreen device only, `Windowed` false at `CreateDevice`
  or `Reset`, by the adapter's `SetGammaRamp` as `Present` draws the window's
  image; a windowed device keeps the ramp for `GetGammaRamp` and shows the
  frame as drawn), `GetAvailableTextureMem` (device-local
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

* Multisampling: `CheckDeviceMultiSampleType` offers 2, 4 and 8 samples with
  one quality level, as DXVK does on most GPUs, and refuses the others and
  `D3DMULTISAMPLE_NONMASKABLE`. `CreateDevice` and `Reset` take the offered
  counts for the back buffer, and `CreateRenderTarget` and
  `CreateDepthStencilSurface` for their surfaces. A count the GPU does not
  have becomes 4, which every Vulkan device has for colour and depth, and
  `GetDesc` reports the count the surface has; the device line says
  `back buffer WxH, N sample(s)`. A multisampled surface is resolved, not
  sampled: `StretchRect` from it resolves (same size and format straight into
  the destination, otherwise through a scratch image and a blit), and so do
  `GetRenderTargetData` and `Present`. A draw whose render target and depth
  surface have different counts is skipped and logged once (D3D9 requires
  them to match). `StretchRect` into a multisampled surface and of a
  multisampled depth surface are refused. The Vita's GXM back end offers no
  multisampling.
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
| `model_lighting` | The model lighting volume's two update paths (`r_altModelLightingUpdate` 0 and 1), with the renderer's own sizes, flags and call order: a MANAGED volume locked whole and patched in place, against a SYSTEMMEM volume locked with `D3DLOCK_NO_DIRTY_UPDATE`, `AddDirtyBox` per cell and `UpdateTexture` into a DEFAULT volume. Two frames of patches; every texel of four cells is drawn from both and must be the same: written cells, a cell written twice, a cell left alone by the second frame, a cell written in the second frame with no `AddDirtyBox` (so it reaches neither). The first `UpdateTexture` uploads the whole new volume, the second only its two cells in one copy, and one with nothing written since uploads nothing. |
| `dirty_update` | `UpdateTexture` copies only what is dirty, in every level: a 16x16 A8R8G8B8 texture of two levels and a 16x16 DXT1 texture, SYSTEMMEM into DEFAULT. A new texture is copied whole; texels written under `D3DLOCK_NO_DIRTY_UPDATE` arrive only inside an `AddDirtyRect`, which reaches level 1 halved and a DXT1 texture grown to whole 4x4 blocks; a lock without the flag marks its own rect. The bytes each update uploads are counted, and each level is drawn 1:1 and read back. |
| `references` | Counts of textures and their levels, `GetContainer`, `QueryInterface`, a released texture that still draws while bound and goes when unbound, the back buffer's and swap chain's own counts (last `Release` returns 0) and the device reference they hold. |
| `threads` | A worker thread creating and filling textures while this thread draws and presents; the last one drawn and read back; nothing left alive. |
| `queries` | Support checks, an event query and an occlusion query finishing (with samples), and an event issued right after `Present` (the engine's swap fence) finishing without a flush while the next frame records; polled with `D3DGETDATA_FLUSH` while its frame is unfinished, it submits the next frame's work once, and the frame goes on with its state and contents. |
| `occlusion` | Occlusion queries count samples exactly, as the engine's sun flare needs: a quad's pixels, two draws, none for a clear, a scissor rectangle, a query begun again, an `END` alone, two queries that overlap, a quad half and wholly hidden by depth, a query over two render targets and over a `Present`, a query released while open. Then 300 queries held at once (more than one pool of Vulkan queries, so a second is made) and one query used and read 400 times (no pool is added: the Vulkan queries given up are used again). Where the device cannot count, every finished query must report 0xffffff. Prints whether it counted, and the Vulkan queries and pools used. |
| `frame_loop` | The engine's frame for 48 frames: draw, poll the swap fence with `D3DGETDATA_FLUSH` until the frame before has finished, issue the GPU sync fence, `Present`, issue the swap fence, while another thread polls the sync fence as the main thread does. At most three submissions a frame, at most one swap fence flush, and the last frame, drawn on both sides of its wait, comes out whole. |
| `gpu_feed` | A frame of 600 draws, each filling a cell of its own, with a scissor rectangle set once that cuts the last column. The adapter submits such a frame in parts while it is drawn ([adapter](vulkan-legacy-adapter.md)): every draw must show whichever part it fell in, the cells no draw touches must keep the clear colour, and the scissor must hold in every part. Prints the number of parts. |
| `tuning` | What a program changes and asks for while its device runs. Five frames of 600 draws, each under a GPU feed setting handed over through `Host::tuning` and taken up by the `Present` before it: off (no parts), after every draw (600 parts), 50 draws a part (2 to 12), 300 draws with an idle count of 10 (the frame starts with the GPU read back, so its first part goes after 10 draws; one to five parts go early, and the picture of that frame is checked on both sides of them), and an idle count no smaller than the plain one (no early part). Every change is logged, the last one back to the device's own setting. `LogDeviceStats` then reports exactly the frames, draws and parts the scene counted since it started the statistics, a second report repeats them and starts again, and a third has no frame in it. Last, another thread changes the setting and asks for the statistics eight times while this one draws and presents, as the engine's main thread does beside its render thread, and the last frame comes out whole. |
| `slow_frames` | The statistics' `slow frame` lines. Eight frames, of which the second makes the only blended draws of the check (so a pipeline is built) and then stands still for 400 ms, and the third uploads a texture and stands still for 250 ms. A report after the first frame has one line, for frame 1 with its 10 draws. The report after the eighth has three, the longest first, and names frames 2 and 3 with their times, their draws, the pipeline and the upload, and with the GPU's times where the device has timestamps. A report that starts the statistics again is followed by one with no such line. The two frames are found by number and not by rank, so a machine busy enough to hold another frame up for longer still passes. |
| `kept_state` | The switch for what a draw takes up from the draw before it (`Tuning::keepDrawState`, the game's `r_vkKeepDrawState`). The same frame of 600 draws, in runs of eight that share a texture and with a vertex shader constant set to another value for every draw, is drawn with the state kept, not kept and kept again. Each change is taken up by the `Present` after it and logged once, the first frame logs none, and the statistics count 600 draws in each. The three pictures are the same to the pixel and every one of the 600 cells has its run's texture (a draw that took the constant of the one before would land in another cell). The frame drawn without the kept state writes close to twice the constants of the other two or more, which write the same as each other, and writes descriptor sets, where the third, kept again, writes none: the sets the first frame wrote last across frames. |
| `multisample` | Multisampled targets. A one-sample triangle is drawn first as the yardstick: no pixel of its edge is between the two colours. A render target of 64x64 at 4 samples is drawn with a 4-sample depth surface of 128x128 (larger, as D3D9 allows) and the triangle in front of a far green quad; `GetDesc` reports 4 samples, an 8-sample target is made with 8 or 4 (`edge_pixels`, `eight_samples_made_with` are printed), and 3 samples are refused. An occlusion query of an 8x8 quad counts 256 samples, four a pixel. A draw with a one-sample depth surface is skipped. `StretchRect` resolves the target into a texture level: inside red, behind green, at least 32 pixels of the edge between the two, and a read of the target gives the same picture as the resolve. A scaled `StretchRect` into a corner of the back buffer. `Reset` to a 4-sample back buffer, a draw with a soft edge in the read back, `Present`, and `GetFrontBufferData` equal to the drawn picture; `Reset` back to one sample. |
| `modern_taa` | The Modern path's temporal anti-aliasing (`ModernSceneDone`, [vulkan-modern.md](vulkan-modern.md)). First the camera reprojection on the CPU: a camera tens of thousands of units from the origin that moves and turns between two frames maps four points (30 to 6000 units away, and a direction for the sky) from this frame to the last within 1e-9 NDC in double and a quarter of a 1920-wide pixel as the float matrix the shader gets; the jitter sequence's first and ninth values. Then frames into the back buffer with a D24S8 depth surface: four that stand still come back exactly as drawn; sixteen with the engine's jitter, whose white quad ends 0.4 pixels past a pixel, leave that pixel between black and white (without the pass it is one or the other) while the pixels inside and outside stay as they were; a scene with no depth surface is refused and left alone. The statistics line at teardown counts 20 scenes, 2 of them without history. |
| `modern_velocity` | The Modern path's velocity for what moves on its own (`ModernSurfaceMoved`, [vulkan-modern.md](vulkan-modern.md#velocity-of-what-moves-on-its-own)). A band of 2-pixel white and black stripes, drawn indexed from 32-byte vertices, moves 2 pixels right each frame behind a static striped quad, for 8 frames three times. Not reported, its stripes go grey under the anti-aliasing (the history comes from the same pixel, which held the other colour); reported with this frame's and last frame's matrices, they stay exactly white and black, and the static quad in front stays exactly as drawn (the band's motion is not written where it is hidden). The third time the band moves in its vertices, as a skinned surface does (`ModernSkinnedSurfaceMoved`): a dynamic buffer locked with DISCARD each frame, the same matrix both frames, drawn from base vertex 2 and minimum index 2. Its stripes stay exact only because the velocity comes from last frame's vertices, which the Modern path kept (drawn from this frame's for both, they go grey). The statistics lines at teardown count 14 scenes with 14 surfaces that move on their own, and 8 scenes that kept the skinned band's 4 KB, 7 of them drawn from in the scene after. |
| `bench` | Only when named (`--scene bench`), never in a full run: 120 frames of 768 draws of four D3D9 calls each, ended as `frame_loop`'s are, while another thread polls the sync fence every 20 microseconds. Prints the time the drawing thread spent on its draw calls per frame and the polls made meanwhile; passes when its frames were presented. |
| `bench_runs` | Only when named, as `bench`: 100 frames of 768 draws in runs of eight that share a texture, an alpha reference and a pixel shader constant, as the engine's draws sorted by material do; each draw sets one vertex shader constant and draws its own quad from one static vertex and index buffer. It measures what the adapter does for a draw like the one before ([adapter](vulkan-legacy-adapter.md)). The last frame is read back and ten of its cells checked. Prints what `bench` prints. |
| `reset` | `Reset` to 96x96 and back: the new size, default state, nothing bound, drawing at the new size. |
| `fullscreen` | `Reset` with `Windowed` false at 96x96 and 60 Hz asks `Host::fullscreen` once for exactly that mode and presents; back to windowed asks once for the desktop's mode (null); windowed again asks nothing. The check's host only records the asks. |
| `present` | `Present` through the device and the swap chain, then `GetFrontBufferData` halfway through the next frame: it must give the frame last shown. |

`scripts/tests/test_vulkan_d3d9_front.py --binary <check>` runs it the way the
adapter's runner does: usage errors, an unknown scene, one scene alone (with no
unsupported call), a run with `KISAK_VK_PRESENT_MODE=fifo` (its swapchain must
be FIFO although the device asks for no vsync), the `bench` scene, the
`bench_runs` scene with the validation layer (where the adapter checks every
block, pipeline and dynamic state it keeps between draws), and a full run in which every scene
matches, the only unsupported call is the deliberate one, frames are presented,
no object outlives its release, the device is destroyed at its last release and
the validation layer reports no errors. It reads the GPU feed's setting from
the device's log line and holds the `gpu_feed` scene's parts to it: none with
the feed off, one for every draw with `KISAK_VK_GPU_FEED=always`, else one to
four. It reads from the same line whether occlusion queries are counted
(always on lavapipe, never with `KISAK_VK_OCCLUSION_QUERIES=0`) and holds the
`occlusion` scene's report to that: at least one Vulkan query and one result
for each of its 700 queries out of two pools, with the results in the
`occlusion queries:` log line too, or none at all. It holds the `tuning` scene's
report to its five frames and 3000 draws, to one to five early parts in the
idle rule's frame, and to the last logged change of the feed being the
setting the device began with. On lavapipe the swapchain of a full run must
have a present mode that does not wait for the display.

Run the script on a machine that is doing nothing else. The `queries` scene's
swap fence check needs its poll to find the frame unfinished, and on lavapipe
a timestamp read at the next frame's start waits for whatever has been
submitted by then. With the CPU heavily oversubscribed (a build running beside
the script), the record thread gets the frame submitted first, each of the
scene's eight tries finds the fence finished, and the script fails `swap fence
poll flushes the next frame once`. Eight busy loops on four cores did that in
10 of 20 runs of the i386 check; an idle machine in none of 20.

The hosted `vulkan_window` job (Linux) stages DXVK Native's headers at the
x86_64 client's pin, builds the check in Debug and RelWithDebInfo and runs that
script on lavapipe with validation and synchronisation checks required. Its
Debug run repeats the script with the GPU feed off and with
`KISAK_VK_GPU_FEED=always` (and the adapter's check with the latter), repeats
both scripts with `KISAK_VK_OCCLUSION_QUERIES=0`, and
builds the check with ThreadSanitizer and runs the `threads`, `frame_loop`
(also with `KISAK_VK_GPU_FEED=always`), `gpu_feed`, `bench`, `occlusion` and
`tuning` scenes, which fail on any data race in our code (llvmpipe's rasterizer threads
are off there and reports from inside the driver are dropped,
[vulkan-core.md](vulkan-core.md)). The
Windows `vulkan-probe` job builds it with MSVC, checks that it imports neither
`vulkan-1.dll` nor `d3d9.dll` and that it starts, and adds it to the
`KisakCOD-vulkan-probe-x64` artifact.

Results so far (2026-11-08, cloud sandbox, no GPU, lavapipe from Mesa 25.2.8):
all 10 scenes pass on x86_64 (Debug and RelWithDebInfo, validation with
synchronisation checks, no errors) and on i386 (Debug, no validation layer:
Ubuntu ships none for i386), with the one deliberate unsupported call.

The `occlusion` scene (2026-11-10, same sandbox, 13 scenes by then) passes on
x86_64 Debug and RelWithDebInfo with validation and on i386 without: every
count exact with the record thread on and off and the GPU feed off, at its
default and after every draw (717 or 718 Vulkan queries for its 700 queries,
from two pools), and 0xffffff from every query with
`KISAK_VK_OCCLUSION_QUERIES=0`. In the game the queries have since run on two
GPUs (laptop run 11 and PC run 7, below); the scene itself has not.

The `tuning` scene (2026-11-10, same sandbox, 14 scenes by then) passes on
x86_64 Debug and RelWithDebInfo with validation and on i386 without, with the
device's own feed off, at its default, at 1, 50 and 300 draws a part and
after every draw, and with the record thread on and off. With the record
thread on, lavapipe took one or two early parts in the idle rule's frame; with
it off, where lavapipe has finished every submission before the call returns,
four or five. ThreadSanitizer finds nothing in the scene. No GPU has run the
scene. The settings have run in the game on two (laptop run 11 and PC run 7,
below), where a real surface also showed its present mode: lavapipe's
offscreen surface offers MAILBOX and FIFO only.

The `slow_frames` scene (2026-11-10, same sandbox) passes its 21 points on
x86_64 Debug and RelWithDebInfo with validation in the seven configurations
the script is run in (the feed at its default, off and after every draw, the
record thread off, static uniforms, static strides with fixed depth compare
and wide targets, occlusion counting off) and on i386 without validation;
alone 20 times in each build, and 20 times beside eight busy loops on the
sandbox's four cores, without a failure. ThreadSanitizer finds nothing in it
with the record thread on or off. On an idle sandbox the two frames it holds
up read 406 to 413 ms and 257 to 265 ms, and beside the busy loops up to 520
ms and 300 ms; nearly all of it shows as GPU time without work before the
frame. No GPU has run the scene. The game has printed the lines on the
Windows PC (PC run 8, below), where the slowest frame of a launch was one
pipeline built during play; what they say about the long frame the laptop
sees about every two minutes is for its next measuring run.

The `kept_state` scene (2026-11-10, same sandbox) passes its 20 points on
x86_64 Debug and RelWithDebInfo with validation in the same seven
configurations and on i386 without validation, and ThreadSanitizer finds
nothing in it with the record thread on or off. Its frame of 600 draws writes
10.0 KB of constants with the state kept and 365.6 KB without. No GPU has run
the scene; the game has run with the state kept and without on the Windows PC
(PC run 8, below). The check also takes `--keep-draw-state 0`, which draws the `bench` and
`bench_runs` scenes without the kept state, so their timings and instruction
counts can be set beside the default's (the [adapter](vulkan-legacy-adapter.md)
has the counts).

The `dirty_update` scene and the new points of `model_lighting` (2026-11-10,
same sandbox) pass on x86_64 Debug and RelWithDebInfo with validation in the
script's configurations, and on i386 without validation. With `UpdateTexture`
copying everything, as it did before, both fail: the unmarked cells and texels
arrive and the byte counts are the whole level's; with a DXT1 rect not grown
to whole blocks, validation fails. In the game this is the model lighting
volume (`r_altModelLightingUpdate 1`, the default): every frame that patched
a few 4x4x4 cells uploaded the whole 1 MB volume, about one 1 MB upload every
other frame in PC run 9; now it uploads the patched cells, 256 bytes each.

The `multisample` scene (2026-11-10, same sandbox, lavapipe, which has 1 and
4 samples) passes its 24 points on x86_64 Debug and RelWithDebInfo with
validation, and the adapter scene answers 2, 4 and 8 samples with one quality
level. The resolved triangle has 188 edge pixels between red and green; the
top row and left column of a quad that fills the target are half covered,
because the half-pixel shift of the viewport (the same as DXVK's) moves the
clip volume by half a pixel. The 8-sample target is made with 4. No GPU has
run the scene; the game has drawn with 4 samples on the Windows PC (PC run 8,
below).

## In the game: r_backend vulkan

`-DKISAK_BUILD_VULKAN_BACKEND=ON` (`scripts/vulkan_probe/vulkan_backend.cmake`,
with `KISAK_BUILD_VULKAN_LEGACY` and the native D3D9 boundary) links the front
into whichever of these programs the build configures:

| Program | How to choose Vulkan |
| --- | --- |
| `KisakCOD-device-probe` | `--backend vulkan [--validation off\|auto\|require]` |
| `KisakCOD-world-draw-probe` | `+set r_backend vulkan` |
| `KisakCOD-mp` | `+set r_backend vulkan` |
| `KisakCOD-sp` | `+set r_backend vulkan` |

The probes are i386, as before; `KisakCOD-mp` is any client architecture and
`KisakCOD-sp` is i386 or x86_64. The x86_64 singleplayer game loads and
draws killhouse, cargoship, blackout, ac130 and airplane through the back end
on the laptop and quits cleanly, with no unsupported call reported
([linux-sp-levels.md](linux-sp-levels.md#vulkan-run-1-laptop-x86_64-2026-11-10)),
and all 21 levels do the same on llvmpipe in WSL
([linux-sp-levels.md](linux-sp-levels.md#vulkan-sweep-all-21-levels-pc-linux-in-wsl-x86_64-2026-11-11)). The
programs still link the system's D3D9 (DXVK Native) and use it by default.
`src/linux/client/vulkan/vulkan_backend.cpp` registers five dvars that are
archived and latched (a change applies at the next `vid_restart`):

* `r_backend`: `d3d9` (default: DXVK on Linux) or `vulkan`.
* `r_vkValidation`: `off` (default), `auto` (when installed) or `require`.
* `r_vkDevice`: -1 (default) lets the core pick a GPU; N picks the Nth device
  the Vulkan driver lists.
* `r_vkShaderCache`: 1 (default) keeps two caches in `<fs_homepath>/vulkan`
  between runs: the translated shaders in `shaders.bin` (the Legacy adapter's
  shader cache), and the Vulkan pipeline cache in `pipelines.bin` (the core's,
  ignored when another device or driver wrote it), and beside them the list
  of pipelines the last launches drew, `pipelines.list`, whose pipelines are
  built on worker threads as the map loads (docs/vulkan-legacy-adapter.md,
  "Pipelines built ahead"). 0 translates every shader and builds every
  pipeline from scratch, at its first draw, at every start.
* `r_visuals`: `legacy` (default: the game as it was made) or `modern`, the
  Modern path, experimental: so far temporal anti-aliasing over the scene
  ([vulkan-modern.md](vulkan-modern.md)). It needs a one-sample back buffer
  (`r_aaSamples 1`); with more the scene is drawn as with `legacy` and the
  reason is logged once.

Four more are for measuring. They are not saved and take effect in the next
frame (the front's `Tuning`), so one launch can compare settings, by hand, in
a config script or with `perf_ab`:

* `r_vkGpuFeed`: `default` (by the kind of GPU, or `KISAK_VK_GPU_FEED`), `off`,
  `on`, or `always` (a part after every draw, for testing).
* `r_vkGpuFeedDraws`: the draws to a part of a frame; 0 (default) leaves the
  device's own count, 128 unless `KISAK_VK_GPU_FEED_DRAWS` says otherwise.
* `r_vkGpuFeedIdleDraws`: the idle rule's count; 0 (default) for no rule.
* `r_vkKeepDrawState`: 1 (default) lets a draw take up the pipeline, the
  constants and the binds the draw before it left (the
  [adapter](vulkan-legacy-adapter.md)'s work per draw); 0 makes every draw
  set them all again. The `frame stats` line shows which is in effect by its
  constants per frame. It is also the switch to try when a picture is wrong
  in a way a stale state would explain: if 0 puts it right, the fault is in
  what is kept.

The `r_vkStats` command (`src/gfx_d3d/r_cmds.cpp`, in the game and the world
draw probe) prints the device's statistics lines since it last ran, or since
the device was made, and starts them again; `r_vkStats keep` prints them
without starting again. To measure a setting: change the dvar, wait for the
frame rate to settle, `r_vkStats`, wait as long as the measurement should
take, `r_vkStats` again and read the second report. With `r_backend d3d9`
the command says there is no Vulkan device. The command exists only where
`r_cmds.cpp` is compiled with `KISAK_VULKAN_BACKEND`: the Linux and macOS
build gives the definition to each program's stand-in objects for it (the
first build with the command did not, and the Linux game had the dvars
without the command until laptop run 11 typed it), and CI's `vulkan_engine`
job looks for the command in the programs it builds.

`scripts/runs/recipes/vulkan-settings-mp-crash.json` measures these settings
on a machine with `perf_run.py` ([runs](runs.md)), in the standing scene
(mp_crash, a fixed view, 12 bots, a slow turn). Each of its two launches
takes the statistics over 120 seconds, with their `slow frame` lines, and
then compares inside the launch, with `perf_ab` in windows of one full turn:
the kept draw state on and off, the GPU feed at its default and off, the idle
rule at 0 and 16 draws, 128 and 32 draws to a part, and last a setting
against itself, which says how far two equal settings sit apart on that
machine. A launch takes about twelve minutes.

With `r_backend vulkan`, `R_HW_Direct3DCreate9`
(`src/gfx_d3d/rhi/r_rhi_d3d9_calls.cpp`, built with `KISAK_VULKAN_BACKEND`)
returns the front and DXVK is never called. There is no fallback: if the Vulkan
device cannot be made, `R_CreateDevice` fails as it would with DXVK. The host
is SDL3, as for DXVK's SDL3 WSI: the `HWND` the Win32 window layer makes is the
`SDL_Window`, the `HMONITOR` is the primary display's `SDL_DisplayID`, the
display modes are SDL's fullscreen modes (or the desktop mode where SDL lists
none, as with its offscreen driver), and the front's log lines go to the
console as `Vulkan: ...`. A fullscreen device makes the window fullscreen on
the primary display at SDL's closest mode (`SDL_GetClosestFullscreenDisplayMode`,
`SDL_SetWindowFullscreenMode`), as DXVK's SDL3 window system does; SDL gives
the desktop its mode back when the window goes windowed, is destroyed or loses
focus. When the device goes, the front logs where the CPU
time went, for comparing loads and first frames with DXVK. The
`vulkan d3d9: device stats: ...` line gives the shaders created and how many
of them were translated (the rest came from the shader cache) with the time
it took, the pipelines built with their time (and of it, the pixel shaders
built for them, each with the pipeline's fixed state folded in), the
pipelines built ahead on worker threads (their time there, the pixel shaders
the workers built for them, how many a draw used, how many a draw waited for
and how long, how many entries the pipeline list holds, and the time spent
queueing them), and the texture uploads with their size and time. The core logs `pipeline cache: N bytes from ...` when it
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

The host is `src/win32/vulkan/win_vulkan_backend.cpp`, with plain Win32, the
same dvars and the `r_vkStats` command:

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
* A fullscreen device switches the primary display to its mode
  (`ChangeDisplaySettingsExW` with `CDS_FULLSCREEN`, left alone when the
  display is there already) and moves the window over the whole display. The
  registry's mode comes back when the device goes windowed or is released.
  `WM_ACTIVATEAPP` calls `Kisak_VulkanAppActivated`: switching away minimises
  the window and then gives the desktop its mode back, coming back sets the
  mode again, as D3D9 does. In the other order (PC run 18, 2026-11-12) Windows
  resized the window to the desktop's full size as the mode changed back, and
  it stayed topmost and drawing instead of minimised. Coming back, Windows
  restored the window at the desktop's full size on the game's smaller mode
  (runs 18b and 18c), because the restore activates the window while it is
  still minimised. So the host sets the restore size on activation, and
  `WM_SIZE` calls `Kisak_VulkanWindowSized`, which moves a fullscreen window
  of the wrong size back over the display while the display is at the game's
  mode (`Vulkan: fullscreen window sized WxH on a WxH display: moved back over
  it`).

Before this (PC run 17, 2026-11-11), `r_fullscreen 1` with `r_mode 1280x720`
left a 2560x1080 display at its mode and 75 Hz and showed the game as a
1280x720 window at `vid_xpos`, `vid_ypos`; Direct3D 9 switched the display to
1280x720 at 60 Hz. The Windows workflow builds it for Win32 Debug and for
x64 (whose artifact is the PC's), checks the configure log says `Vulkan back
end: linked into KisakCOD-mp`, and checks the Win32 executable does not import
`vulkan-1.dll`. The Win32 Release player build leaves it out, except in a
manual run of the workflow with the `vulkan` input set: that run's Release
artifact, `KisakCOD-Release-Vulkan`, carries it with the same two checks, so a
PC run can compare the two back ends in an optimised build. Debug code slows
the back end's own work, which runs in the game's executable, far more than
the system's `d3d9.dll`, which is optimised in either build. Hosted runners have no GPU, so CI never draws the game through
it. On a Windows PC with the game files (RTX 2080 Super, Win32 Debug builds
from CI, 2026-11-08; not CI evidence) the game draws mp_crash on Vulkan and
quits cleanly. From one indoor view, Vulkan and the real `d3d9.dll` agree at
43.9 dB in windowed desktop captures and 37.2 dB in exclusive-fullscreen
screenshots; repeating the same comparison gives 37.6 to 43.9 dB, so that
is the noise of the view.
With the shader cache, a warm start reaches the map in 15 s against 62 s
cold; the pipeline cache saves about 0.3 s more on that driver. Reports:
`renderer/vulkan-backend-pc-run-{1,2,5}.md` in the project files.
PC run 6 (2026-11-10, 8aee1d43, same PC) ran the record thread and the GPU
feed there for the first time: the game starts, draws mp_crash and quits
cleanly in every row with no validation error. Its frame rates say nothing
about them, though: every Vulkan row sat at exactly 75 frames a second, the
display's rate, in a window with vsync off (`swapchain 1024x768
B8G8R8A8_UNORM MAILBOX, 3 images`), while the real `d3d9.dll` ran the scene
at 215. The core asked for MAILBOX before IMMEDIATE then; it now asks for
IMMEDIATE first ([vulkan-core.md](vulkan-core.md)). Report:
`renderer/vulkan-backend-pc-run-6.md`.
PC run 7 (2026-11-10, 2b8fde19, same PC, the hosted Win32 Debug artifact, the
machine alone) confirmed that the cap sat on the present mode. The swapchain
line says `IMMEDIATE, 3 images` and the scene runs at 154.6 frames a second
over 60 s; with `KISAK_VK_PRESENT_MODE=mailbox` it is back at 74.9; the real
`d3d9.dll` read about 203 (182 to 230 in twelve overlay samples). So in that
Debug build Vulkan reaches about three quarters of Direct3D 9. What the frame
holds there: 526 draws, 9.1 passes, 231 descriptor sets and 467 KB of
constants, with the GPU busy for 1.0 ms in passes of a 6.5 ms frame, so that
PC waits for the CPU and not for the GPU. The feed settings, in windows of
20 s inside one launch, two each: off 163.8 frames a second, the default
(every 128 draws) 161.1, which is inside the spread of its own two windows,
and the idle rule at 16 draws 150.2, about 8% lower. A launch builds 132
pipelines in 1.09 s. The sun's occlusion query gives the answers real
Direct3D 9 draws by (see [the laptop](#the-game-on-the-laptop) for the
method): 256.0 samples on average and no empty result with the sun in the
open (`setviewpos -257 177 219 240 -50`), every result empty indoors, one
result a frame, and screenshots whose mean colour, in the centre and over
the whole picture, is within one level of Direct3D 9's at both places.
Nobody watched the window, so tearing with IMMEDIATE is not judged. Report:
`renderer/vulkan-backend-pc-run-7.md`.
PC run 8 (2026-11-10, bc299ba6, same PC, the hosted Win32 Debug artifact, the
machine alone, the scene of run 7 with 12 bots) ran the kept draw state,
descriptor sets kept across frames and multisampling on a GPU for the first
time. Every launch quit cleanly. With the state kept the scene runs at 171.2
frames a second (167.3 in a repeat at the end) and writes no descriptor set
and 101 KB of constants a frame; with `r_vkKeepDrawState 0` it runs at 160.8
and writes 227 sets and 454 KB. The kept sets are given up once a launch, at
the map's load. The real `d3d9.dll` read 209.5, so this Debug build reaches
about 80% of Direct3D 9, up from about three quarters in run 7. With
`r_aaSamples 4` both back ends print `Using 4x anti-aliasing`, the device
line says `4 samples`, Vulkan runs at 175.8 and Direct3D 9 at 230.0 (four
samples cost nothing measurable on that GPU; the Direct3D 9 pair differs by
its launch-to-launch spread), and screenshots from the same view read a mean
luma of 87.4 and 87.0 with every cell of a 5x5 grid within 3. The GPU is
busy for about 0.9 ms in passes of a 5.8 ms frame, so the PC still waits for
the CPU. The slowest frame of the four Vulkan launches was 126 ms, one
pipeline built in the window in 116.75 ms; every other slow frame was 13 to
18 ms. Report: `renderer/vulkan-backend-pc-run-8.md`.
PC run 9 (2026-11-10, 1883bf69, same PC and scene, the machine alone) is the
first in an optimised build: the Win32 Release artifact of a manual `vulkan`
run. Vulkan runs at 451.2 and 462.0 frames a second and the real `d3d9.dll`
at 479.6 and 481.9, so Vulkan is about 5% behind, where the Debug build of
run 8 put it about 20% behind: most of that gap was the back end's own code
running unoptimised. With `r_vkKeepDrawState 0` Vulkan runs at 435.6, about
4% below its default, writing 281 descriptor sets and 595 KB of constants a
frame against none and 129 KB. A frame holds about 700 draws and 10.5
passes; the GPU is busy for 0.4 ms in passes and idle for 1.3 ms before the
frame, so the PC still waits for the CPU. The slowest frame of a window was
7 to 12 ms (one pipeline built during play in 7.41 ms). A launch's first
frame takes about 1.08 s, of which 45 to 50 ms is its 6,200 or so texture
uploads (about 190 MB) and nearly all the rest the GPU waiting for work, and
the 71 pipelines of the first view take 340 ms with an empty cache and 89 ms
from it. Over a launch the device uploads 14 to 17 GB of textures in about
27,000 to 30,000 uploads, most of them 1 MB, which is close to one every
other frame during play. Every launch quit cleanly. Report:
`renderer/vulkan-backend-pc-run-9.md`.
PC run 10 (2026-11-10, 953d94c7, the Release artifact of PR 217's manual
run, same PC and scene, the machine alone) measures `UpdateTexture` copying
only the patched cells. A launch now uploads about 374 MB in about 27,000 to
37,000 uploads, where run 9's uploaded 9 to 17 GB; the first frame is about
half of that (173 to 189 MB), and no window of play has a 1 MB upload (the
one left is in the frame that builds the first view's pipelines, before the
window). Vulkan runs at 473.4, 457.4 and 477.9 frames a second and the real
`d3d9.dll` at 474.6 and 497.7: means of 469.6 against 486.2, about 3%
behind where run 9 had about 5%, though the launches of each spread as far
as that. The GPU is busy for about 0.9 ms of a frame, so the PC still waits
for the CPU. Report: `renderer/vulkan-backend-pc-run-10.md`.
PC run 11 (2026-11-11, c2db610c, master after PR 232, the Release artifact,
the machine alone) measures the shaders of runs 18 and 19 there: Vulkan
runs at 477.6, 460.1 and 458.4 frames a second (mean 465.4, run 10 469.6),
so the shorter shaders do not move the PC, which waits for the CPU. The GPU
is busy 0.80 ms of a frame (run 10 0.90). Against the real `d3d9.dll` the
answer depends on how it is counted: its two launches read 471.0 and 446.3
by the counters (Vulkan 1.5% ahead) and 485.7 and 459.3 by the game's frame
log (Vulkan 1.4% behind), and its launches differ 5 to 6% from each other.
Vulkan's slowest frames are longer (25 to 97 ms against 7 to 8 ms), one of
them two pipelines built during play. The first launch with an empty cache
spent 3.4 s building 131 pipelines, where run 10 spent 1.1 s on 133.
Report: `renderer/vulkan-backend-pc-run-11.md`.
PC run 12 (2026-11-11, eb316f35, after PRs 233 and 234) shows the slow
first launch was NVIDIA's own shader cache, which keeps compiled shaders
whatever the game's folder holds: the first view's 71 pipelines took 1.29 s
with every cache cold, and 0.25 s in a second fresh folder whose pipeline
cache was empty but whose driver cache now had the shaders. Vulkan ran at a
mean of 470.8 frames a second against Direct3D 9's 478.5, inside the spread
of their launches, and a 103 ms frame showed up on Direct3D 9 too. PC run
12b put the depth compare read at run time (`KISAK_VK_DEPTH_COMPARE=dynamic`)
beside the default on that build: no change on this CPU-bound PC (461 to
485 frames a second either way), and the sun's occlusion query still one a
frame. Reports: `renderer/vulkan-backend-pc-run-12.md`,
`renderer/vulkan-backend-pc-run-12b.md`.

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
* Feed settings side by side (run 11, 2026-11-10, 2b8fde19, on mains in
  power-saver mode, twelve windows of 27 s in one launch, two for each
  setting): the idle rule at 16 draws 137.8 fps and at 32 draws 135.8 (each
  beside the default's part every 128 draws), a part every 32 draws 137.4,
  every 64 draws 125.8, every 128 draws (the default) 127.4, off 125.5; DXVK
  in a launch of its own 137.5. The two windows of a
  setting lie 4 to 12 fps apart for every setting but off, whose two were
  next to each other: as far as the settings lie from one another, so this
  is no ranking. The GPU lines say more: with the feed off the GPU is busy
  for 6.36 ms in passes and without work for 1.69 ms before a frame; at the
  default it is without work for 0.71 ms but busy for 7.04 ms. The feed ends
  a pass to submit and begins it again, so a frame has more of them (6.4 with
  the feed off and 8.6 at the default in the launches of runs 9 and 10). It
  gets back a millisecond of idle GPU and the passes take 0.7 ms longer; why
  they do on this driver is not known.
  `scripts/runs/recipes/vulkan-settings-mp-crash.json` compares the settings
  with `perf_ab` instead, in windows of 9 s in the order A B B A, which a
  drift over minutes moves less.
* Anti-aliasing: up to laptop run 11 the back end had no multisampled
  targets. Its `CheckDeviceMultiSampleType` offered none, so the engine drew
  one sample a pixel on Vulkan whatever `r_aaSamples` said
  (`R_SetupAntiAliasing` steps down to none), where DXVK gives the samples
  asked for. The back end now has them (above), so a build with them prints
  `Using 4x anti-aliasing` on Vulkan too. The laptop's
  profile has `r_aaSamples 4` (read in run 11), and the launch lines of the
  runs above set no value of their own. The DXVK rows of runs 10 and 11
  print `Using 4x anti-aliasing` and the Vulkan rows print no such line, so
  every frame rate above sets a Vulkan frame of one sample beside a DXVK
  frame of four, and the gap is larger than it reads.
  `scripts/runs/recipes/vulkan-aa-mp-crash.json` measures the two alike, in
  the order V1 D1 V4 D4 D4 V4 D1 V1 (back end and `r_aaSamples` on the
  command line), each row keeping the engine's anti-aliasing line where
  there is one; on a build without multisampled targets the V4 rows draw one
  sample. The Windows PC's runs set `r_aaSamples 1` for both back
  ends, so its numbers do compare.
* Settings in one launch (run 12, 2026-11-10, 473f14d6, mains, two launches
  of `vulkan-settings-mp-crash.json`): 130.9 and 136.6 fps over 120 s. In
  `perf_ab` windows the kept draw state is worth 1.8 and 2.9% (off costs
  0.8 ms of CPU a frame), the GPU feed 7.7 and 10.5% (off is slower: the
  default helps), and the idle rule at 16 draws and a part every 32 draws
  move less than a setting measured against itself (3.6 and 9.3%), so
  differences under about 4% are noise there. The slowest frame, 428.6 ms
  after 432 s of play, built 4 pipelines that were not in the cache (423.7
  ms of it): pipelines built during play are the long frame the laptop sees.
* The same samples (run 13, 2026-11-10, 5871c201, mains): with 4 samples
  each, Vulkan runs at 100.7 and 95.7 fps and DXVK at 152.5 and 151.1, so
  Vulkan is about 35% behind; with one sample each, 135.8 against 176.4,
  about 23% behind (a last pair was void: another program woke up). Both
  print `Using 4x anti-aliasing` with 4. The GPU is busy in passes for 6.7
  to 8.4 ms of a one-sample Vulkan frame and 9.5 to 12.5 ms with 4, so the
  laptop waits for the GPU, and the gap is the GPU's time on our frame,
  not the samples. Screenshots of the two back ends at 4 samples match.
* Where the GPU's time goes (run 14, 2026-11-11, one sample, Mesa's
  `INTEL_MEASURE` over 9 frames; DXVK's half crashed under it): about 17 ms
  of GPU work a frame while measured, nearly all of it its 900 or so draws
  in about 9 passes. One pass of about 360 draws takes about 8.8 ms, the
  next largest 4.4 and 3.4 ms. Colour and depth clears that ANV does the
  slow way, about 2 of each a frame, take 466 us, about 2.7% (a first,
  partial read put it at 6%); they were all `vkCmdClearAttachments`, which
  ANV cannot do as a fast clear. Clears of whole targets are now a pass's
  load operation ([adapter](vulkan-legacy-adapter.md#how-a-draw-works)).
  Report: `renderer/vulkan-backend-laptop-run-14.md`.
* Clears as load operations (run 15, 2026-11-11, a13e9109, run 13's scene
  at 4 samples, defaults C against `KISAK_VK_PASS_CLEARS=0` O, C O O C):
  about 60% of the clears became a pass's load operation (29,727 of 49,988),
  none with the switch. C ran at 98.3 and 85.7 fps, O at 92.0 and 93.6: the
  means (92.0 and 92.8) are within the noise of the two C rows, so the 2.7%
  of slow clears gives no fps the laptop can see, and does no harm. GPU time
  per frame 9.3 to 10.4 ms either way. Report:
  `renderer/vulkan-backend-laptop-run-15.md`.
* Pass timestamps (run 16, 2026-11-11, 1f15b84a, the same scene at 4
  samples, defaults C against `KISAK_VK_PASS_TIMESTAMPS=0` T, C T T C): C ran
  at 107.6 and 106.1 fps, T at 107.1 and 107.5, with GPU frames of 8.53 and
  8.68 ms against 8.53 and 8.54. The pairs around every pass cost the laptop
  nothing it can measure, so they stay on. Report:
  `renderer/vulkan-backend-laptop-run-16.md`.
* Shader code (run 17, 2026-11-11, the same scene at one sample, held 60 s
  with Mesa's `INTEL_DEBUG=vs,fs` and its shader cache off, summed by
  `renderer/tools/intel_shader_stats.py`): neither side spills. Our pixel
  shaders' widest variants have a median of 635 instructions and 17,080
  cycles against DXVK's 359 and 9,038; our vertex shaders 545 and 5,458
  against 154 and 536 (DXVK's figures as corrected after its log was read
  whole). The counts are per shader compiled, not per draw. Part
  of it is the multiply rule: every multiply checked both factors for zero
  where DXVK multiplies plainly on Intel, and the fast rule is now the
  default ([shaders](vulkan-legacy-shaders.md#float-rules)). Report:
  `renderer/vulkan-backend-laptop-run-17.md`.
* Multiply rule (run 18, 2026-11-11, build ee6fe8d2, run 13's scene at 4
  samples, F S S F): the fast rule (F) ran 129.4 and 128.2 fps with GPU frames
  of 7.04 and 7.14 ms, the strict one (S) 105.8 and 105.5 fps with 8.61 and
  8.72 ms: 22% more frames and 1.6 ms less GPU time, about 19 times the spread
  of F's two rows. Mesa's statistics of F: vertex median 151 instructions and
  625 cycles (DXVK 154 and 536), pixel 457 and 15,292 (DXVK 359 and 9,038), no
  spills. The picture at run 13's ground view, by numbers only (mean luma,
  5x5 grid, black and white shares, isolated pixels): F against DXVK differs
  less than F against its own repeat; the sun view could not separate the
  renderers. Report: `renderer/vulkan-backend-laptop-run-18.md`.
* Alpha test and legacy state (run 19, 2026-11-11, build d2481792, the same
  scene at 4 samples, N A L L A N): defaults (N) 172.6 fps with a GPU frame
  of 4.90 ms; the alpha test read at run time (A, `KISAK_VK_ALPHA_TEST=dynamic`)
  139.6 fps and 6.42 ms; the legacy state read at run time (L,
  `KISAK_VK_LEGACY_STATE=dynamic`) 158.1 fps and 5.43 ms. The two costs
  roughly add up to N's gain over run 18's F (43.8 fps). Mesa's statistics of
  N: pixel median 232 instructions and 7,947 cycles (DXVK 359 and 9,038),
  vertex 114 and 434, no spills, 226 pixel shaders (more variants, one per
  alpha compare). Then N against DXVK in one sitting (N D D N): 170.5 against
  166.8 fps, Vulkan 2% ahead, about four times the larger row spread. The
  picture at the ground view, by numbers only, lands on one of a few states
  per launch whichever back end or switch draws it (DXVK and Vulkan share
  one); the differences, up to 0.4 luma in a cell, follow the launch, not the
  switches. Report: `renderer/vulkan-backend-laptop-run-19.md`.
* Depth compare (run 20, 2026-11-11, build d6b2a634, the same scene at 4
  samples): fixed per pipeline (N, the default) against read at run time (D,
  `KISAK_VK_DEPTH_COMPARE=dynamic`). The repeat that waited for a quieter
  desktop (N D D N, every row captured twice) gave N 173.2 fps with a GPU
  frame of 5.09 ms and D 162.0 fps with 5.48 ms: every N capture (170.8 to
  176.0) beat every D capture (161.5 to 162.7). The first sitting, with the
  desktop's load uneven between rows, pointed the same way in every pairing.
  The branch costs about 0.4 ms of GPU time a frame. Mesa's statistics of N:
  pixel median 157 instructions and 2,634 cycles (run 19: 232 and 7,947; DXVK
  359 and 9,038), vertex unchanged at 114 and 434, no spills, 183 pixel
  shaders (run 19: 226; DXVK 169). The picture at the ground view, by numbers
  only, falls on one of the states run 19 found. Report:
  `renderer/vulkan-backend-laptop-run-20.md`.
* Present mode: the laptop's swapchain still says `MAILBOX, 4 images` with
  vsync off now that the core asks for IMMEDIATE first (run 11), so its
  surface does not offer IMMEDIATE.
* The sun (run 11): the engine draws a sprite of 16 by 16 pixels at the sun's
  place inside one occlusion query a frame and takes the share of its samples
  that pass as how much of the sun is in view (`rb_sky.cpp`). `r_vkStats`
  prints what the queries returned, so the check needs no picture: look
  along `r_lightTweakSunDirection` (`-50 240 0` on mp_crash: yaw 240, pitch
  -50) and read the `occlusion queries:` line of a stretch of five seconds.
  With the sun in the open (`setviewpos -716 461 600 240 -50`)
  every result is 256 samples, also with `r_aaSamples 4`; indoors
  (`setviewpos 1504 512 200 240 -50`) and under a roof slab (the same place
  at z 295) every result is empty; one result a frame. Screenshots reduced to
  numbers on the laptop match DXVK's within 0.1 in mean brightness and 0.5 in
  every cell of a 5x5 grid, and with `KISAK_VK_OCCLUSION_QUERIES=0` the
  indoor view shows the old fault: an almost black frame with a bright
  centre. Neither back end draws a visible flare at those places (on the PC
  real Direct3D 9 does not either), so the pictures prove that the two agree
  and the line proves the count.
* Picture: models drew dark where the profile had
  `r_altModelLightingUpdate 0`, because the adapter dropped a texture's
  contents when it was locked again; fixed, and run 7 matches DXVK at 40.1 dB
  there. With `r_altModelLightingUpdate 1` (the default) run 7 had models 2.7
  to 6.7 luma darker than through DXVK (33.6 dB, one screenshot each). That is
  not a fault of this back end: on the Windows PC (RTX 2080 SUPER, PC run 6)
  real Direct3D 9 and Vulkan, each at 0 and at 1, draw the same brightness.
  Five launches with three screenshots each gave a mean luma between 63.50
  and 63.90 for all 15, and no cell of a 4x4 grid differs between any two
  settings by more than it moves inside one launch. The laptop, measured
  again the same way (model lighting laptop run 1: DXVK at 0, at 1 and at 1
  again, Vulkan at 0 and at 1), shows no difference either: no cell of a 4x4
  or an 8x8 grid is beyond its spread in any pair, and Vulkan against DXVK
  at 1 (39.5 dB) is closer than DXVK against its own repeat (36.8 dB). Run
  7's gap was the view moving: one launch's three shots there read a mean
  luma of 65.70, 65.84 and 67.05. DXVK 2.7.1 itself gives the same texels on
  both paths (a plain Direct3D 9 program with random patches,
  `renderer/model-lighting/model_lighting_d3d9.cpp` in the project files:
  0 of 9.8 million texels differ on the laptop's DXVK and Intel driver).
* Comparing screenshots of that indoor view (`setviewpos 1504 512 200 90 0`
  on mp_crash): one pair says little. Parts of the view move, and each launch
  lands in one of a few states: on the PC a plain repeat of the same launch
  was 31.1 dB from its twin, while two shots inside one launch are about 50
  dB apart. Compare the mean luma per grid cell over several shots of each
  launch, with a repeated launch as the baseline
  (`renderer/model-lighting/luma_grid.py` in the project files).

Reports: `renderer/vulkan-backend-laptop-run-{4,5,6,7,8,9,11,12,13,14,15,16,17,18,19,20}.md`,
`renderer/vulkan-backend-pc-run-{6,7,8,9,10,11,12,12b}.md` and
`renderer/model-lighting/laptop-run-1.md` in the project files.

## Next

The laptop's gap to DXVK is closed: at the same 4 samples Vulkan draws run
13's scene 2% faster than DXVK (laptop run 19), after the fast multiply rule
(run 18), the alpha test fixed per pipeline and the legacy state the game
never changes held as constants ([shaders](vulkan-legacy-shaders.md#legacy-state)).
Run 13 had it 35% behind. The depth compare fixed per pipeline
([adapter](vulkan-legacy-adapter.md#depth-compare)) took the pixel shaders'
median to 157 instructions against DXVK's 359 (run 20), and Vulkan now
compiles 183 pixel shaders against DXVK's 169. On the Windows PC, in a
Release build, Vulkan is level with Direct3D 9 within the spread of their
launches (PC run 11), and the PC waits for the CPU: what is left there is
the time per frame, the front's lock first. Then the pipelines: built during
play they are the long frames on both machines (laptop run 12, PC run 11).
A first launch after the shaders change builds them slower while the
driver's own cache is cold (PC run 12).
