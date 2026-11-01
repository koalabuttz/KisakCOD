# Linux free camera: frame ownership, camera and input

Status on 2026-11-01: **groundwork only.** The camera, its key bindings and the
frame owner exist and are covered by asset-free tests. Nothing draws a map yet: the
engine-renderer static map view and the native D3D9 boundary are separate work in
progress, and this layer is written to sit on top of them without depending on
either. No hardware, map or device result is claimed here.

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
| `tests/linux_free_camera/` | Asset-free tests, run by `scripts/tests/run_free_camera_checks.py` |

None of these files is in a CMake target yet. Following the project rule, each
joins a Linux target only with its own compile proof, which today is the
standalone runner (Clang, `-m32`, production client flags, `-Werror`) in Debug and
RelWithDebInfo. The production unit `src/universal/com_math_anglevectors.cpp` is
compiled unmodified beside them.

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

The static map view work plans to implement it as: `LoadMap` loads the map zone
with the production DB loader and calls `R_LoadWorld`; `UnloadMap` is
`R_ShutdownWorld` plus the zone unload; `Device`/`ResetDevice` use the renderer's
own lost-device and reset path; `DrawFrame` zeroes a `refdef_s`, sets
`localClientNum` 0, calls `FillRefdef`, then `R_BeginFrame`, `R_RenderScene`,
`R_EndFrame` and `R_IssueRenderCommands`. Unsupported materials or techniques
return `false` with a reason rather than being skipped silently.

`FillRefdef` writes only the camera's fields (rectangle, both half-FOV tangents,
origin, axis, `zNear`, `time`) and is a template, so this layer never includes a
renderer or D3D9 header and never puts a native DXVK type into an engine
structure. The owner never asks for a view at an empty extent, so
`R_RenderScene`'s positive width/height/tangent assertions hold.

## Input integration (next step, not done)

The existing window host already delivers ordered engine events. The plan:

- after `ClientInputHost::Dispatch` drains an `SE_KEY`, forward it to
  `FrameOwner::Key` only when no menu or console holds input;
- on each host tick, pass `ClientInputHost::ConsumeMouse()` (the production
  `CL_MouseEvent` accumulation) to `FrameOwner::Mouse`;
- `WindowHost::RelativeMouseActive()` drives `SetLookCaptured`, focus events drive
  `Focus`, `SurfaceExtent()` (pixels, not logical size) drives `Resize`, and
  `vidConfig.aspectRatioWindow` drives `SetAspect`;
- `FrameDeadline` supplies `msec`; quit and signals call `Shutdown` before the
  window and renderer are destroyed.

## What is verified

Asset-free, in this checkout (not yet hosted CI), Clang 18 `-m32`, Debug (`-O0`)
and RelWithDebInfo (`-O2`), and again with GCC 13 under ASan/UBSan:

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

- No map is loaded or drawn, no real device is reset, and nothing is compared with a
  captured game frame. Those wait for the static map view and the D3D9 boundary.
- The window/input integration above is not written.
- No hosted CI run has executed the new job yet.

## Acceptance once the static map view lands

Run on named hardware with a private map, keeping screenshots and exports out of
the repository:

1. Replay reference traversal A (forward, mouse turn left 818 counts, fast forward,
   rise) and B (four arrow-key turns) through the real renderer; record the pose and
   a frame hash at fixed checkpoints.
2. Resize during traversal, minimize and restore, and force a device reset at a
   checkpoint; the frame after each must match the uninterrupted run at that pose.
3. Reload the map mid-traversal and switch maps; the pose is kept and no resource
   from the first load survives.
4. Quit during a draw and by signal; shutdown order is unload then renderer.
5. Then widen draw coverage in this order, each with its own fail-closed refusal
   and evidence: static models, effects, transparency/sorting, shadows and
   post-processing. Faithful appearance first; enhanced visuals stay optional.
