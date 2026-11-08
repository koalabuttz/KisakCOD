# Vulkan D3D9 front

Opt-in, part of the [Legacy adapter](vulkan-legacy-adapter.md)'s build
(`-DKISAK_BUILD_VULKAN_LEGACY=ON`). `src/gfx_vk/d3d9` implements `IDirect3D9`,
`IDirect3DDevice9` and the resource interfaces on the adapter, so the
renderer's own Direct3D 9 calls, unchanged, draw through Vulkan. A check
program, `KisakCOD-vulkan-d3d9-check`, drives it through those interfaces and
checks what comes back. Nothing of the engine is compiled in yet; the next step
makes `R_HW_Direct3DCreate9` return the front's `IDirect3D9` when `r_backend`
is `vulkan`.

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
                                        `-- the D3D9 front (r_backend vulkan, next step)
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
volume texture use their texture's counts (`GetContainer` returns it); the back
buffer and the implicit swap chain use the device's. The device itself goes once
its own count and its live objects are both zero; its last `Release` drops every
binding first. The check counts all of this (`references` scene, teardown).

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
| `references` | Counts of textures and their levels, `GetContainer`, `QueryInterface`, a released texture that still draws while bound and goes when unbound, the back buffer and swap chain counting as the device. |
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
script on lavapipe with validation and synchronisation checks required. The
Windows `vulkan-probe` job builds it with MSVC, checks that it imports neither
`vulkan-1.dll` nor `d3d9.dll` and that it starts, and adds it to the
`KisakCOD-vulkan-probe-x64` artifact.

Results so far (2026-11-08, cloud sandbox, no GPU, lavapipe from Mesa 25.2.8,
validation with synchronisation checks): all 8 scenes pass in Debug and
RelWithDebInfo, one deliberate unsupported call, no validation errors. The
front also compiles for i386 against the boundary.

## Next

`r_backend vulkan`: `R_HW_Direct3DCreate9` returns the front with an SDL host,
the multiplayer client and the world draw probe link it, and mp_crash is drawn
through it and compared image by image with DXVK on the laptop and the PC.
