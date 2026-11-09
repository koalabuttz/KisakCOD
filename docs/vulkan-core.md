# Vulkan core and window probe

Opt-in, off by default. This is the base of the planned Vulkan renderer back
end: the shared Vulkan core (`src/gfx_vk`) and `KisakCOD-vulkan-probe`, a small
program that opens an SDL3 window and clears, presents and resizes it through
the core, or draws a small textured scene through it and checks the pixels.
Nothing of the engine is compiled into either, no game file is read, and the
default build does not change.

## Where it fits

The renderer plan puts one engine-independent Vulkan core under two users: the
Legacy adapter, which will sit behind the D3D9-shaped seam in
`src/gfx_d3d/rhi/r_rhi.h`, and the Modern path. The core never sees materials,
techniques, D3D9 types or shader bytecode, so it does not depend on which shader
translator is chosen; that choice only affects the Legacy adapter. The seam's
signatures still use D3D9 types until its interface freeze (plan step M1b), so
nothing in the renderer calls the core yet.

The core has what both users need to draw (plan section 3.2), behind a small
C++ API in `vk_core.h`. Users that build pipelines and descriptor sets get the
device and the function table (`Functions`, `DeviceHandle`) and record Vulkan
commands themselves; memory, lifetime and synchronisation stay in the core.

Start-up (`vk_core.cpp`):

| Piece | How |
| --- | --- |
| Loader | Not linked. The window system hands the core `vkGetInstanceProcAddr` (SDL3 opens the loader), and every function is looked up through it (`vk_dispatch.h`). A machine without Vulkan fails with a message, not a dynamic-linker error. |
| Instance | Vulkan 1.3. The window system's surface extensions, portability enumeration where offered (MoltenVK), and the Khronos validation layer on request. With validation on, synchronisation validation is on too. |
| Device | Needs Vulkan 1.3, dynamic rendering, synchronization2, timeline semaphores, scalar block layout (for the [Legacy shaders](vulkan-legacy-shaders.md)' uniform blocks), the Vulkan memory model, buffer device addresses and demote-to-helper (all three used by the Legacy shaders' SPIR-V and required by Vulkan 1.3), `VK_KHR_swapchain` and one queue that draws and presents to the window. Picks discrete, then integrated, then virtual, then CPU devices, or the index given. Turns on anisotropic filtering, wireframe fill, independent blend, clip distances and 16-bit shader floats where the device has them (`Features` says which). Picks the depth-stencil format: D24S8 where it can be an attachment and sampled, else D32S8. |
| Swapchain | `B8G8R8A8_UNORM` or `R8G8B8A8_UNORM` in sRGB-nonlinear colour space: UNORM because the retail renderer writes gamma-space values and never asks for sRGB conversion. FIFO with vsync, otherwise mailbox, then immediate. Rebuilt when the surface is out of date or suboptimal, or when the window reports a new size; a minimised window skips frames. |
| Swapchain images | Wrapped as core images, so passes, blits and read-back treat them like any other image. Copyable and blit targets where the surface allows. |

Memory (`vk_memory.cpp`):

| Piece | How |
| --- | --- |
| Allocation | Pooled blocks per memory type (64 MB device, 16 MB mapped), first fit with coalescing; buffers and images never share a block, so `bufferImageGranularity` never applies. Requests over half a block get their own allocation. Only Upload and Readback memory is mapped, even where every type is host-visible (integrated GPUs): a 32-bit build keeps its address space for the game. |
| Buffers | Device (written through staging), Upload (mapped, CPU writes) and Readback (mapped, cached where possible). |
| Images | 2D, cube and 3D, with levels; a view of the whole image for attachments and copies, and for sampled images a second view shaders sample, with the image's swizzle (`ImageDesc::sampleSwizzle`, so one-channel formats read as D3D9's L8 or A8) and the depth aspect only; usage as asked (sampled images can also be copied into). |
| Lifetime | `Destroy*` waits for every submission recorded so far: objects go once the timeline passes that point, so a frame still using one on the GPU is safe. Pipelines and shader modules follow the same rule. |

Frames (`vk_frame.cpp`):

| Piece | How |
| --- | --- |
| Recording | A command buffer is always open, so work recorded outside a frame (a map load's uploads) needs no frame; it goes with the next `Flush` or `Present`. Every submission signals a timeline semaphore with the next serial, which is how the core knows what the GPU has finished; `SubmissionSerial` and `SerialFinished` give users the same answer for memory they recycle themselves. |
| Frames | `BeginFrame` opens the frame; the swapchain image is acquired the first time `SwapchainImage` asks for it (the Legacy adapter does at its final blit), so the frame's work is recorded before the core has to wait for the present engine. `Present` hands the image over, submits and presents; a frame whose acquire found the swapchain out of date submits its work and is skipped. Two frames in flight: after a present the next frame slot waits for its previous use and recycles its command buffers, staging, transient memory, descriptor pools and queries. |
| Record thread | See below. |
| Image state | Each image's layout and last access are tracked; passes, copies, uploads and `UseImage` record the synchronization2 barriers they need (none for a read after a read in the same layout). A pass cannot record barriers, so images it samples are made Sampled first; images written by uploads go back to Sampled before the next pass on their own. |
| Passes | Dynamic rendering with up to four colour attachments and a depth-stencil attachment, clear or load. A clear or don't-care load discards the old contents (no layout transition cost). |
| Uploads | `StageBufferWrite` and `StageImageWrite` return staging memory and record the copy at once, so it is ordered with the work around it. Buffer copies share one barrier before and one after a run of them. Rewriting a texture level already written since the last barrier gets a barrier first; other levels do not. A frame slot that holds 64 MB of staging flushes and waits instead of growing (a long map load). |
| Transient memory | Per-frame host-visible memory for vertex, index and uniform data. |
| Descriptor sets | Allocated from per-frame pools, reset when the slot comes round again. |
| Copies and read-back | Blits between any level and layer of two images (scaling and format conversion), and read-back of a level of any colour image (every slice of a 3D one, compressed formats as blocks), the swapchain image included, waited on by `WaitReadback`. |
| Timing | GPU timestamps around every pass, kept per pass name (min, max, average), and around each whole frame. |

Recording (`vk_record.cpp`):

Users record commands with `Record(core, f)`, where `f(cmd, vk)` makes the
`vkCmd*` calls; the core records its own barriers, copies, passes and
timestamps the same way, and its command buffer begins, submissions and
presents as bookkeeping packets. Without the record thread (the default) each
one runs at once. With `CoreDesc::recordThread` (the D3D9 front sets it;
`KISAK_VK_RECORD_THREAD=0` or `1` overrides it) the core starts a thread of its
own: `Record` copies `f` into 64 KB chunks, and that thread runs them in order.
This is DXVK's design: the render thread no longer pays for the driver's
recording, which was about 2.5 ms of each laptop frame
(`renderer/vulkan-backend-laptop-run-6.md` in the project files).

| Piece | How |
| --- | --- |
| Split | The owning thread keeps everything that decides what is recorded: layouts and barriers, staging and transient memory, descriptor sets (allocated and written there, bound by a packet), serials, timestamps' bookkeeping and deferred destruction. The record thread owns the command pools' use: allocating, beginning, recording, ending, resetting; and the queue: `vkQueueSubmit2` and `vkQueuePresentKHR`. |
| Packets | `f` is captured by value and must be trivially destructible; structures holding pointers (`VkDependencyInfo`, `VkRenderingInfo`) are built inside it, pointing at the captured copies. A chunk goes to the thread when it fills and at every submission. |
| Waiting | `WaitSerial` first waits until the thread has submitted that serial; `WaitIdle`, swapchain rebuilds, `DetachWindow` and `DestroyCore` wait until it has run everything. The swapchain is the thread's while it presents, so the acquire waits until the previous frame's present has been made: late in the frame, so normally not at all. The time the owning thread waits is `CoreStats::recordWaitMs`. |
| Errors | A failed call on the thread stops it running packets; the owning thread fails with that message at its next wait, submission or frame. Validation messages raised on the thread are kept and logged, and counted, by the owning thread at its next sync; every other caller's are logged at once. |
| Lifetime | Objects a packet uses must live until it has run. `Destroy*` waits for the GPU to finish the submission, which comes after the thread has run it, so the rule is unchanged. |

Pipelines (`vk_pipeline.cpp`):

| Piece | How |
| --- | --- |
| Pipeline cache | Read at start-up from `CoreDesc::pipelineCachePath` when its header names this device and driver, written back (to a temporary file, then moved) by `DestroyCore` unless it holds exactly what was read. |
| Pipelines | Graphics pipelines through the cache, with pipeline creation feedback: the core counts pipelines, cache hits, CPU time, and pipelines built during a frame, which the plan's target says should be none during play. |
| Samplers | One per distinct description, owned by the core. |

The probe's frames cycle slowly through colours, so a person watching sees that
frames are live. With `--draw` they show a small scene instead: a textured
quad that wobbles slowly, a grey quad in front of it, and a magenta one. Escape
or closing the window ends a run early; that still passes if at least one frame
was presented.

### The draw scene (`--draw`)

`src/gfx_vk/tools/vk_probe_draw.cpp` uses every resource path of the core each
frame: a device-local vertex and index buffer written through staging; a
64x64 texture with four levels uploaded one by one; a 256x256 offscreen colour
target and a depth target; a pipeline from SPIR-V built before the first frame
through the pipeline cache; a descriptor set per frame with the transform in
transient memory; a quad whose vertices are written into transient memory
every frame and sampled at an explicit level; a blit of the target into the
swapchain. Every frame it also creates, fills and destroys a buffer, and every
30 frames it rewrites the smallest texture level, which the same frame samples.

At the end it draws one still frame and reads back the target and the
swapchain image, and compares seven points of known colour (within 2 per
channel): the clear colour, the texture's four quadrants (level 0), a grey
quad drawn first but nearer, which the depth test must keep in front of the
textured quad drawn after it, and the quad sampled at level 2.

The probe's two shaders are GLSL in `src/gfx_vk/tools/shaders`, compiled into
`src/gfx_vk/tools/vk_probe_shaders.h` by `scripts/tools/vulkan/gen_spirv_header.py`
(needs `glslangValidator`), so no shader compiler is needed to build the probe.
The header records each source's SHA-256; `gen_spirv_header.py --check` fails
when a source changed without regenerating the header, and runs `spirv-val` on
the embedded words.

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

On macOS the probe builds with `-DKISAK_PLATFORM=macos` and runs on Metal
through MoltenVK; see [macos.md](macos.md#vulkan-window-probe).

## Running

```sh
KisakCOD-vulkan-probe [--frames N] [--size WxH] [--validation off|auto|require]
                      [--device N] [--no-vsync] [--resize] [--check-capture]
                      [--list-devices] [--draw] [--pipeline-cache PATH]
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
- `--draw` draws the scene above every frame and checks the still frame's
  points (in the swapchain too with `--check-capture`). It fails if a pipeline
  was built during a frame.
- `--pipeline-cache PATH` reads the pipeline cache from `PATH` and writes it
  back at the end.

Output is `key=value` lines on stdout ending in `vulkan_probe=passed` or
`vulkan_probe=failed stage=<stage>` with a `vulkan_probe.reason=` line. Exit
status 0 is a pass, 1 a failed stage, 2 a usage error. The stages are `sdl`,
`loader`, `window`, `core`, `frames`, `resize`, `draw`, `capture` and
`validation`. Each run also prints its pass and frame GPU times, submissions,
memory, upload and descriptor counts, and pipeline counts.

To see it on a desktop, run it with no options: a 960x540 window cycles colours
for 300 frames (about five seconds with vsync).

## Checks

`scripts/tests/test_vulkan_window_probe.py --binary <probe>` runs the probe
several ways: bad arguments exit 2; `--list-devices` finds a usable device; with
no Vulkan driver visible it fails closed (stage `loader` or `core`); a full run
with `--resize --check-capture` passes, follows the resize, reads back the
cleared colour, times the pass and has no validation errors; and two `--draw`
runs sharing a pipeline cache file pass every point in the target and the
swapchain, build their pipeline before the first frame, time the scene pass,
run uploads, transient memory, descriptor sets and deferred destruction, and
have no validation errors, the second one starting from the cache file the
first wrote.

The hosted CI job `vulkan_window` builds the probe for x86_64 in Debug and
RelWithDebInfo and runs that script on Mesa's lavapipe (a CPU Vulkan driver)
through SDL's offscreen video driver, which supports Vulkan through
`VK_EXT_headless_surface`. It needs no GPU, display server or game data, and
requires the validation layer with synchronisation checks. It also runs
`gen_spirv_header.py --check`. One validation message, the depth-compare format
rule the layer checks statically against the Legacy shaders, is counted apart
from errors (`validation_ignored`; see
[vulkan-legacy-adapter.md](vulkan-legacy-adapter.md#the-validation-layer-and-depth-compare)).

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

The macOS workflow's `vulkan_probe` job builds the probe for arm64 and x86_64
against Homebrew's MoltenVK and runs the same script in a Cocoa window on the
hosted runners' paravirtualized Metal GPU. Both pass with validation and
synchronisation checks on (2026-11-07, MoltenVK 1.4.2), the first runs on a
GPU of any kind; the Intel VM needs `MVK_CONFIG_USE_METAL_ARGUMENT_BUFFERS=0`
([macos.md](macos.md#vulkan-window-probe)).

The record thread (2026-11-09, cloud sandbox, lavapipe from Mesa 25.2.8): the
probe, the Legacy adapter check and the D3D9 front check pass with the thread
on and off, x86_64 with validation and synchronisation checks and i386 without;
the device probe draws its frames through the engine with it on. The CI job
runs each check again with the thread switched the other way, and under
ThreadSanitizer the D3D9 front's `threads` and `frame_loop` scenes, a probe run
with draws, read-back and swapchain rebuilds and the Legacy adapter check, all
with the thread on: no races in the core, the adapter or the front. Those runs
set `LP_NUM_THREADS=0`. Lavapipe is not built with ThreadSanitizer, which then
sees only the pthread calls the driver makes and reports a race inside it: one
of lavapipe's threads destroys a mutex and a condition variable that an
llvmpipe rasterizer thread locked and signalled (3 of 40 `frame_loop` runs
here, every run of the adapter check, with or without the record thread).
Without rasterizer threads lavapipe's other threads and every thread of ours
are still there.

The clear-and-present probe has run on both physical GPUs (2026-11-07): the
Windows PC's RTX 2080 Super and the laptop's Intel ANV, i386 and x86_64 (report
in the project files, `renderer/vulkan-probe-laptop-run-1.md`). The draw scene
has so far run on lavapipe only (2026-11-08, cloud sandbox, x86_64 Debug, Mesa
25.2.8, validation with synchronisation checks: all points match, no errors).

## Next

The [Legacy adapter](vulkan-legacy-adapter.md) now sits on the core, with the
[Legacy shader translator](vulkan-legacy-shaders.md) turning the game's shaders
into SPIR-V for it, and its [D3D9 front](vulkan-d3d9-front.md) implementing
the D3D9 interfaces the renderer calls. Next: the `r_backend vulkan` switch,
which hands the renderer the front at device creation, and mp_crash drawn
through it, compared with DXVK on the laptop and the PC. Not in the core yet, because
nothing needs them so far: compute pipelines, memory budgets
(`VK_EXT_memory_budget`), and multisampled targets.
