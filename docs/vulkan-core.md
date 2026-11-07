# Vulkan core and window probe

Opt-in, off by default. This is the first piece of the planned Vulkan renderer
back end: the shared Vulkan core (`src/gfx_vk`) and `KisakCOD-vulkan-probe`, a
small program that opens an SDL3 window and clears, presents and resizes it
through the core. Nothing of the engine is compiled into either, no game file is
read, and the default build does not change.

## Where it fits

The renderer plan puts one engine-independent Vulkan core under two users: the
Legacy adapter, which will sit behind the D3D9-shaped seam in
`src/gfx_d3d/rhi/r_rhi.h`, and the Modern path. The core never sees materials,
techniques, D3D9 types or shader bytecode, so it does not depend on which shader
translator is chosen; that choice only affects the Legacy adapter. The seam's
signatures still use D3D9 types until its interface freeze (plan step M1b), so
nothing in the renderer calls the core yet.

What the core does today:

| Piece | How |
| --- | --- |
| Loader | Not linked. The window system hands the core `vkGetInstanceProcAddr` (SDL3 opens the loader), and every function is looked up through it (`vk_dispatch.h`). A machine without Vulkan fails with a message, not a dynamic-linker error. |
| Instance | Vulkan 1.3. The window system's surface extensions, portability enumeration where offered (MoltenVK), and the Khronos validation layer on request. With validation on, synchronisation validation is on too. |
| Device | Needs Vulkan 1.3, dynamic rendering, synchronization2, `VK_KHR_swapchain` and one queue that draws and presents to the window. Picks discrete, then integrated, then virtual, then CPU devices, or the index given. |
| Swapchain | `B8G8R8A8_UNORM` or `R8G8B8A8_UNORM` in sRGB-nonlinear colour space: UNORM because the retail renderer writes gamma-space values and never asks for sRGB conversion. FIFO with vsync, otherwise mailbox, then immediate. Rebuilt when the surface is out of date or suboptimal, or when the window reports a new size; a minimised window skips frames. |
| Frames | Two frames in flight, one command pool, fence and acquire semaphore each, and one render-done semaphore per swapchain image. |
| Pass | One dynamic-rendering pass that clears the swapchain image, with synchronization2 barriers. |
| Timing | GPU timestamps around the pass, read when the frame's fence has signalled (min, max, average). |
| Read-back | On request, the next presented frame is copied to host memory for checks. |

The probe's frames cycle slowly through colours, so a person watching sees that
frames are live. Escape or closing the window ends a run early; that still
passes if at least one frame was presented.

## Building

Needs SDL3 3.2.10 or newer and the Vulkan headers (1.3 or newer):

```sh
cmake -S . -B build-vulkan -G Ninja -DKISAK_PLATFORM=linux \
  -DKISAK_LINUX_ARCH=x86_64 -DCMAKE_BUILD_TYPE=RelWithDebInfo \
  -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++ \
  -DKISAK_BUILD_VULKAN_PROBE=ON -DSDL3_DIR=<sdl3-sdk>/lib/cmake/SDL3
cmake --build build-vulkan --target KisakCOD-vulkan-probe
```

`KISAK_LINUX_ARCH=i386` works the same with a 32-bit SDL3. Headers come from
`libvulkan-dev` or the Vulkan SDK; `-DKISAK_VULKAN_INCLUDE_DIR=<dir>` points at
any other Vulkan-Headers include directory. The runtime needs a Vulkan loader
and driver for the same architecture (on Debian and Ubuntu, `libvulkan1` and
`mesa-vulkan-drivers`, with `:i386` for the 32-bit build).

On Windows the probe builds on its own with MSVC, without the game or the
DirectX SDK, from `scripts/vulkan_probe/CMakeLists.txt` (the Linux tree includes
the same `scripts/vulkan_probe/vulkan_probe.cmake`):

```sh
cmake -S scripts/vulkan_probe -B build-vulkan -A x64 \
  -DCMAKE_PREFIX_PATH=<sdl3-sdk> -DKISAK_VULKAN_INCLUDE_DIR=<Vulkan-Headers>/include
cmake --build build-vulkan --config Release --target KisakCOD-vulkan-probe
```

The build copies `SDL3.dll` next to the executable. The GPU driver supplies
`vulkan-1.dll`; the validation layer needs the Vulkan SDK.

## Running

```sh
KisakCOD-vulkan-probe [--frames N] [--size WxH] [--validation off|auto|require]
                      [--device N] [--no-vsync] [--resize] [--check-capture]
                      [--list-devices]
```

- `--list-devices` prints every physical device and why it can or cannot be used.
- `--validation auto` (the default) loads the validation layer when it is
  installed and loads, `require` fails when it does not, `off` never loads it.
  Any validation error fails the run. Messages from the loader (a driver that is
  installed for another architecture, say) are printed but do not count.
- `--resize` grows the window halfway through the run and checks that the
  swapchain followed it.
- `--check-capture` clears the last frame to a known colour, reads it back and
  compares every pixel in memory. Nothing is written to disk.

Output is `key=value` lines on stdout ending in `vulkan_probe=passed` or
`vulkan_probe=failed stage=<stage>` with a `vulkan_probe.reason=` line. Exit
status 0 is a pass, 1 a failed stage, 2 a usage error. The stages are `sdl`,
`loader`, `window`, `core`, `frames`, `resize`, `capture` and `validation`.

To see it on a desktop, run it with no options: a 960x540 window cycles colours
for 300 frames (about five seconds with vsync).

## Checks

`scripts/tests/test_vulkan_window_probe.py --binary <probe>` runs the probe
several ways: bad arguments exit 2; `--list-devices` finds a usable device; with
no Vulkan driver visible it fails closed (stage `loader` or `core`); and a full
run with `--resize --check-capture` passes, follows the resize, reads back the
cleared colour, times the pass and has no validation errors.

The hosted CI job `vulkan_window` builds the probe for x86_64 in Debug and
RelWithDebInfo and runs that script on Mesa's lavapipe (a CPU Vulkan driver)
through SDL's offscreen video driver, which supports Vulkan through
`VK_EXT_headless_surface`. It needs no GPU, display server or game data, and
requires the validation layer with synchronisation checks.

Results so far (2026-11-05, cloud sandbox, no GPU, lavapipe from Mesa 25.2.8):

| Build | Window system | Validation | Result |
| --- | --- | --- | --- |
| x86_64 Debug | SDL offscreen | on, with synchronisation checks | pass |
| x86_64 Debug | SDL X11 on Xvfb | on, with synchronisation checks | pass |
| i386 Debug, RelWithDebInfo | SDL offscreen | off: Ubuntu ships the layer for 64-bit only | pass |

Synchronisation validation found one hazard while this was written: the last
barrier handed the image to the present engine with destination stage `NONE`,
which falls outside the render-done semaphore's signal scope
(`SYNC-HAZARD-PRESENT-AFTER-WRITE`). The barrier and the signal now use the same
stage (`kPresentHandOffStage` in `vk_core.cpp`).

The Windows workflow's `vulkan-probe` job builds the probe with MSVC for x64
(SDL3 at the Linux jobs' pin, Vulkan-Headers v1.3.296), checks that it does not
import `vulkan-1.dll` and that it starts, and uploads it with `SDL3.dll` as the
`KisakCOD-vulkan-probe-x64` artifact. Hosted Windows runners have no GPU, so it
does not run there.

### GPU runs (2026-11-07)

These are local results from two developer machines, not CI evidence. Neither
machine has the Khronos validation layer installed, so validation was off in
every run below: **synchronisation validation has not yet run on a real GPU**,
only on lavapipe in CI.

| Machine | GPU and driver | Build | Window system | Result |
| --- | --- | --- | --- | --- |
| Laptop | Intel RPL-U iGPU, Mesa 25.0.7 ANV, Vulkan 1.4.305 | Linux x86_64 and i386, RelWithDebInfo (`459543f`) | Wayland and SDL offscreen | pass |
| Windows PC | NVIDIA RTX 2080 SUPER, driver 621.14, Vulkan 1.4.357 | Windows x64 Release, CI artifact (`a2178a4`) | Win32 | pass |

The probe passed every check on both machines:

- the device listing and a full run with `--resize --check-capture`: the
  swapchain followed the resize to 1088x612 and the capture matched the clear
  colour with 0 mismatched pixels;
- an uncapped run with `--no-vsync`;
- on the laptop, `scripts/tests/test_vulkan_window_probe.py --validation auto`
  (all 14 checks) on both SDL offscreen and Wayland.

| Machine | Build | Vsync run (FIFO) | `--no-vsync` run (MAILBOX) | Clear pass GPU ms, avg |
| --- | --- | ---: | ---: | ---: |
| Laptop | x86_64 | 48.7 fps | 838.9 fps | 0.17 |
| Laptop | i386 | 50.2 fps | 1195.3 fps | 0.17 |
| Windows PC | x64 | 75.5 fps (74 Hz monitor) | 76.5 fps | 0.015 |

On the PC, windowed MAILBOX stays paced by the 74 Hz display, so the
`--no-vsync` run there is not uncapped. With `--validation require`, the probe
stops at the core stage with a clear message when the layer is missing (exit 1).

## Next

Further core pieces from the renderer plan, in roughly this order: memory and
resource lifetime with deferred destruction, uploads, pipelines from SPIR-V
with an on-disk pipeline cache and a compile counter, descriptors, a pass list
that places the barriers, and offscreen targets. The Legacy adapter starts after
the seam's interface freeze.
