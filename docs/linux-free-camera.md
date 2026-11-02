# Linux free camera: frame ownership, camera and input

Status on 2026-11-02: **camera, frame owner and window input exist; not yet wired
to the renderer.** The camera, its key bindings, the frame owner and the window
input that feeds them are covered by asset-free tests. The engine renderer draws
a real map in the opt-in [world draw probe](linux-world-draw-probe.md) (#23); the
`SceneRenderer` adapter that connects that renderer to this owner is being built
by the static map view work and is not here. No hardware, map or device result is
claimed for this layer.

## Why a host camera

The milestone after a static map view is moving through that map. Player spawn
needs cgame, snapshots, prediction and the authentication boundary, which are
later work. A host-owned camera lets the real renderer be exercised under
continuous motion, resize, device reset, map reload and shutdown first, and then
be widened to models, effects and transparency, without pretending a player
exists.

It is not player movement: no `playerState_s`, no pmove, no collision, no bob or
view effects. It does use the engine's conventions so a later switch to the cgame
view changes the source of the numbers, not their meaning.

## Files

| File | Role |
| --- | --- |
| `src/linux/client/view/free_camera.{h,cpp}` | Pose, mouse/key turning, movement, projection, `FillRefdef` |
| `src/linux/client/view/free_camera_controls.{h,cpp}` | Engine key codes (`SE_KEY`) and pointer counts to movement intent |
| `src/linux/client/view/frame_owner.{h,cpp}` | Frame order, `SceneRenderer` contract, map and shutdown lifetime |
| `src/linux/client/view/free_camera_window.{h,cpp}` | The renderer's own window, adopted: its keys, pointer, focus, extent and quit into the owner |
| `tests/linux_free_camera/` | Asset-free tests: the camera and owner by `scripts/tests/run_free_camera_checks.py`, the window input by `scripts/tests/test_linux_window.py` |

The camera, controls and owner are compiled by the standalone runner (Clang,
`-m32`, production client flags, `-Werror`) in Debug and RelWithDebInfo. The window
input needs SDL, so it is compiled in the opt-in window host build
(`KISAK_BUILD_LINUX_WINDOW_HOST`) as `KisakCOD-free-camera-window-test`, with SDL2
and SDL3, which is the compile proof for all four view sources in a CMake target.
No shipped binary links them yet. The production unit
`src/universal/com_math_anglevectors.cpp` is compiled unmodified beside them in
both places.

## Conventions and where they come from

| Behaviour | Source in the engine | Free camera |
| --- | --- | --- |
| Angles to axis | `AnglesToAxis` (`universal/com_math_anglevectors.cpp`) | Called directly; axis[1] is left |
| Move directions | `AngleVectors` | Called directly; right is AngleVectors' right |
| Mouse turn | `CL_MouseMove` with `cl_mouseAccel` 0, freelook | yaw -= `m_yaw` x `sensitivity` x dx; pitch += `m_pitch` x `sensitivity` x dy |
| Key turn | `CL_AdjustAngles` | `cl_yawspeed`/`cl_pitchspeed` degrees per second |
| Pitch limits | `player_view_pitch_up/down` (85) | Clamped, never wrapped from input |
| Projection | `CG_UpdateFov` | tanY = tan(fov/2) x 0.75, tanX = tanY x aspect |
| Near plane | MP `refdef.zNear = 0` | 0, the renderer's `r_znear` default |
| Step cap | client `frame_msec` cap | 200 ms; a hitch is not a teleport |
| Speed | `g_speed` (190) | Units per second; shift multiplies by 4 (host-only) |

Movement is direct: the normalized wish direction times speed times time. That is
deliberately simpler than `PM_NoclipMove` (no acceleration or friction), so
traversal is frame-rate independent and exactly predictable in tests.

Fixed host bindings: `w`/`s` forward/back, `a`/`d` strafe, `space`/`c` up/down,
`shift` fast, arrow keys turn. Opposing keys cancel.

## Frame order

`FrameOwner::Frame(msec, time)` does, in order:

1. Take pending pointer counts and held keys. Without focus there is no intent;
   without a loaded map the input is discarded so a load never starts with a burst.
2. Turn (mouse, then keys) and move the camera.
3. Ask the renderer for device state. `Lost` skips the frame. `NeedsReset` resets
   first; `StillLost` retries next frame; `Failed` is terminal.
4. Apply the latest resize, once, coalesced. An empty extent (minimize) never
   reaches the renderer; frames are skipped until a real extent returns.
5. Draw and present from the camera view, or skip with a reason
   (`no-map`, `minimized`, `device-lost`).

Fail-closed: a failed resize, reset or draw, or any renderer exception, moves the
owner to `Failed`. After that only `UnloadMap` and `Shutdown` reach the renderer.
`Load` always unloads the previous map first and releases held input; `Reload`
keeps the pose. `Shutdown` unloads, then shuts down, exactly once, also when it is
requested from inside a draw (it then runs when the draw returns) and again from
the destructor. Calls come from one thread; a re-entrant load from a draw is
refused.

## The renderer contract

`SceneRenderer` is the only thing the owner knows about rendering:

```cpp
bool LoadMap(const std::string &map, std::string &error);
void UnloadMap() noexcept;
bool Resize(ViewExtent extent, std::string &error);
DeviceStatus Device();                   // Ready, Lost, NeedsReset
ResetResult ResetDevice(std::string &);  // Reset, StillLost, Failed
bool DrawFrame(const CameraView &view, std::string &error);
void Shutdown() noexcept;
```

The adapter planned on top of the world draw probe implements it as: `LoadMap`
loads the map with `CM_LoadMap` and `R_LoadWorld`; `UnloadMap` is
`R_ShutdownWorld` plus the zone unload; `Device`/`ResetDevice` use the renderer's
own lost-device and reset path; `DrawFrame` zeroes a `refdef_s`, sets
`localClientNum` 0, calls `FillRefdef`, then the probe's frame sequence
(`R_BeginFrame`, `R_RenderScene`, `R_EndFrame`, `R_IssueRenderCommands`).
Unsupported materials or techniques return `false` with a reason rather than being
skipped silently.

Points for that adapter, from the probe as merged:

- The renderer runs single-threaded there (`sys_smp_allowed` off before
  `R_RegisterDvars`). The owner is single-threaded too and never calls the
  renderer from another thread.
- The probe sets `tanHalfFovY = tanHalfFovX x height/width` (fixed 65-degree
  horizontal). `FillRefdef` uses `CG_UpdateFov` instead (fixed vertical, horizontal
  widens with aspect). The two agree at 4:3 and differ at other aspects, so a
  probe digest is not a reference for this camera except at 4:3.
- The probe draws at `vidConfig.sceneWidth/Height`. The adapter's `Resize` should
  leave the scene extent equal to the extent it was given, so the view rectangle
  the owner passes matches the renderer's scene.
- A view takes a few draws before the static model cache settles. The owner
  draws every frame anyway; acceptance below must compare settled frames, not
  frame one.
- The renderer must present one frame without a world after `R_Init` and after
  any device reset before it draws a world, or that world frame's batched
  surfaces are drawn from no index buffer
  ([why](linux-world-draw-probe.md#the-first-frame)). The probe does this with an
  empty frame; the adapter's `LoadMap`, `Resize` and `ResetDevice` must keep it.

`FillRefdef` writes only the camera's fields (rectangle, both half-FOV tangents,
origin, axis, `zNear`, `time`) and is a template, so this layer never includes a
renderer or D3D9 header and never puts a native DXVK type into an engine
structure. The owner never asks for a view at an empty extent, so
`R_RenderScene`'s positive width/height/tangent assertions hold.

## Window input

The renderer's window is the one `R_CreateWindow` makes through
`src/linux/client/window/win32_window_sdl3.cpp`; DXVK presents to it, and nothing
else pumps its events (the world draw probe and the planned adapter never do).
`FreeCameraWindow` adopts that window with `WindowHost`'s adopted mode
(`AdoptedWindow`) and is the only event pump in the process:

- The host pumps the window's SDL events through the same translation as the
  window host (physical US key positions, sub-pixel pointer carry, focus and
  pointer boundaries). It never creates a software surface on the window, never
  presents to it and never destroys it. Its SDL video reference is its own, so the
  window layer's reference survives it.
- Keys go to `FrameOwner::Key`, pointer counts to `FrameOwner::Mouse`. Look
  follows the window's actual relative capture: a left click in the focused
  window asks for capture, Escape releases it, and Escape without capture asks to
  quit. Those three presses are not passed on.
- Focus loss releases held keys and capture (the host keeps the request and
  re-applies it when focus returns); nothing is pressed again on focus gain.
- Each tick gives the owner the window's pixel size, or an empty extent while the
  window is minimized (tracked from minimize, restore and focus events, starting
  from the window's flag), so the owner skips draws instead of resizing to
  nothing. The adapter's `Resize` resets the back buffer and scene size to exactly
  that extent and never resizes the window.
- The caller times frames (`Tick(wait, msec, time)`, with `msec` from
  `FrameDeadline` or a clock; the owner caps a step at 200 ms). The aspect stays
  the owner's default, width/height, until the driver passes
  `vidConfig.aspectRatioWindow` through `FrameOwner::SetAspect`.
- Quitting (Escape, window close, `RequestQuit`) only stops the loop. The order on
  the way out is: destroy `FreeCameraWindow` (capture and keys released, window
  left alone), then `FrameOwner::Shutdown` (unload, then the renderer, which may
  destroy the window).

There is no console or menu in this loop, so no key catcher decides who owns a
key. When one is added, keys must go through `ClientInputHost` and its catchers
before they reach the camera.

## What is verified

Window input, asset-free, Clang 18 `-m32`, the dummy video driver of SDL 3.2.10
(built as CI builds it) and of Ubuntu 24.04's SDL2 2.30, locally in Debug: `free_camera_window` adopts a window made the way the engine's window
layer makes one and drives the owner through synthetic SDL events, with the same
recording renderer: adoption without a surface, present or destroy; keys and
pointer counts; click to capture, Escape to release, Escape to quit; focus loss;
minimize and restore; a real window resize reaching the renderer once in pixels;
another window's events ignored; quit and window close leaving the renderer
alone; teardown releasing held input before the owner shuts down. Both dummy
drivers grant relative capture; a refusal is injected at the SDL call (linker
`--wrap`) to check that it is reported with SDL's reason and look stays off.
Fourteen deliberate defects in the adopted host and the bridge were each made in
turn and each was caught.

Camera and owner, asset-free, Clang 18 `-m32`, Debug (`-O0`) and RelWithDebInfo (`-O2`): locally,
locally again with GCC 13 under ASan/UBSan, and in the hosted `free_camera` job
([run 41327486090](https://github.com/koalabuttz/KisakCOD/actions/runs/41327486090),
both configurations, on the pre-merge head 5144bf4):

- `free_camera`: 106 checks: tuning defaults equal the dvar defaults, validation,
  the production axis convention at fixed and 2000 random poses, mouse and key
  turning, pitch clamping and wrapping, movement directions, diagonal and fast
  speed, the step cap, frame-rate independence, the origin bound, the projection
  (checked against the cgame's own precomputed 65-degree constant), refdef filling
  that leaves other fields untouched, and the bindings.
- `frame_owner`: 114 checks against a recording renderer that flags any call out of
  contract: resize coalescing and minimize, a minimized start, device loss with a
  retried and a successful reset, a failed reset, failed resize and draw and a
  thrown exception (fail-closed), load failure and recovery, reload and map switch,
  repeated, deferred and destructor shutdown, focus and capture rules, a foreign
  thread, two scripted reference traversals with closed-form expected poses, and a
  seeded run of 40 owners x 500 random operations.
- Eight deliberate defects in the owner (drawing while the device is lost,
  loading without unloading, shutting down without unloading, resending an
  unchanged extent, focus loss keeping keys held, ignoring pointer capture,
  running a deferred shutdown inside the draw, and drawing after a failure) were
  each made in a scratch copy and each was caught. The runner's own contract test
  repeats the first one on every run.

## What is not verified

- No map is loaded or drawn through this owner, no real device is reset, and
  nothing is compared with a captured game frame. That waits for the adapter.
- The window input has only seen SDL's dummy driver: no real desktop's focus,
  minimize or relative-capture behaviour, and no window made by `R_CreateWindow`
  itself.

## Acceptance once the adapter lands

Run on named hardware with a private map, keeping screenshots and exports out of
the repository:

1. Replay reference traversal A (forward, mouse turn left 818 counts, fast forward,
   rise) and B (four arrow-key turns) through the real renderer; record the pose and
   a frame hash at fixed checkpoints, after the view has settled by the world
   draw probe's rule.
2. Resize during traversal, minimize and restore, and force a device reset at a
   checkpoint; the frame after each must match the uninterrupted run at that pose.
3. Reload the map mid-traversal and switch maps; the pose is kept and no resource
   from the first load survives.
4. Quit during a draw and by signal; shutdown order is unload then renderer.
5. Then widen draw coverage in this order, each with its own fail-closed refusal
   and evidence: static models, effects, transparency/sorting, shadows and
   post-processing. Faithful appearance first; enhanced visuals stay optional.
