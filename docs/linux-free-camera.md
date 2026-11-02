# Linux free camera: frame ownership, camera and input

Status on 2026-11-02: **camera, frame owner and window input exist, and the
frame owner drives the engine renderer on a GPU with a real map.** The camera, its
key bindings, the frame owner and the window input that feeds them are covered by
asset-free tests. The `SceneRenderer` adapter over the engine renderer the
[world draw probe](linux-world-draw-probe.md) brought up is written
([below](#the-renderer-contract)), with a replay that drives the owner through
the reference traversals on a real map ([the replay](#the-replay)). Both build
in the opt-in `KisakCOD-world-draw-probe` target and fail closed without a
device. The replay passed on one laptop GPU with `mp_crash`
([what is verified](#what-is-verified)). The
[window driver](#the-window-driver) joins the adapter, the owner and the window
input: the camera flown through the renderer's own window, live or by a checked
script of SDL events. Its scripted run has passed on the same laptop under
Wayland and X11, and David has flown the live camera there.

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
| `src/linux/tools/engine_scene_renderer.{h,cpp}` | `EngineSceneRenderer`, the `SceneRenderer` over the engine renderer |
| `src/linux/tools/free_camera_replay.cpp` | The replay of the reference traversals on a real map |
| `src/linux/tools/free_camera_window_driver.cpp` | The window driver: the renderer's window adopted, flown live or by a checked script |
| `src/linux/tools/world_draw_engine.{h,cpp}` | Frame, readback, map load and unload, shared with the world draw probe |

The camera, controls and owner are compiled by the standalone runner (Clang,
`-m32`, production client flags, `-Werror`) in Debug and RelWithDebInfo. The window
input needs SDL, so it is compiled in the opt-in window host build
(`KISAK_BUILD_LINUX_WINDOW_HOST`) as `KisakCOD-free-camera-window-test`, with SDL2
and SDL3, which is the compile proof for all four view sources in a CMake target.
The camera, controls and owner also build, with the adapter, the replay, the
window input (`free_camera_window.cpp` and the SDL3 window host it adopts the
window with, `window_host.cpp` and `pixel_buffer.cpp`) and the window driver, in
the opt-in `KisakCOD-world-draw-probe` target (`KISAK_BUILD_WORLD_DRAW_PROBE`),
with the client definitions and `-Wall -Wextra -Werror`. No shipped binary links
them yet. The production unit `src/universal/com_math_anglevectors.cpp` is
compiled unmodified beside them in every place.

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

`EngineSceneRenderer` (`src/linux/tools/engine_scene_renderer.h`) implements it
over the engine renderer that `R_Init` started, on the thread that started it:

| Call | What the engine does |
| --- | --- |
| `LoadMap` | The map zone (`DB_ZONE_GAME`), `CM_LoadMap`, `CM_LinkWorld`, `Com_LoadWorld`, `R_LoadWorld`, `R_EndRegistration`: the cgame's map load, as the probe does it. The map must be allowlisted. On failure every part already loaded is released again. |
| `UnloadMap` | `R_ShutdownWorld`, `Com_ShutdownWorld`, `CM_Shutdown`, then the map zone is freed (a zone list with no name and `DB_ZONE_GAME` as the free flags). The asset owners refuse to unload a world still in use, so the order is fixed. |
| `Resize` | `dx.windows[0]` (which `R_ResetDevice` reads) and the video config (`R_StoreWindowSettings`) take the extent as both display and scene size, then the device is reset as below. The window is never resized or moved here: that is the window host's. |
| `Device` | `TestCooperativeLevel`: `D3D_OK` is ready, `D3DERR_DEVICELOST` lost, `D3DERR_DEVICENOTRESET` needs a reset; any other answer throws, and the owner fails closed. |
| `ResetDevice` | `R_RecoverLostDevice`'s sequence, call for call: release the lost images, materials and world buffers, `R_ResetDevice`, reload them. `R_RecoverLostDevice` itself cannot be called, because `R_CanRecoverLostDevice` asserts that the device says it is lost. A device that is still lost answers `StillLost`. |
| `DrawFrame` | A zeroed `refdef_s` (local client 0, `R_InitPrimaryLights`), `FillRefdef`, then the probe's frame: `R_BeginFrame`, `R_ClearScene`, `R_SetLodOrigin`, `R_RenderScene`, `R_EndFrame`, `R_IssueRenderCommands`. The view must cover the whole scene at the renderer's extent. |
| `Shutdown` | `UnloadMap`, then `R_Shutdown(1)`, the client's renderer shutdown: graphics zones, render targets, the window and the device. |

DXVK never reports a windowed device lost, so `RequestReset()` makes the next
`Device()` answer `NeedsReset` once; the replay uses it. `Window()` returns the
`SDL_Window*` `R_CreateWindow` made, for a window host to adopt; it is valid
until `Shutdown`.

Points the adapter keeps:

- One thread: the renderer runs with `sys_smp_allowed` off, and the owner never
  calls it from another thread.
- After `R_Init` and after every device reset, the backend's pre-tessellated
  index buffer is unset until a frame has been presented
  ([why](linux-world-draw-probe.md#the-first-frame)). The constructor, `Resize`
  and `ResetDevice` each present one empty frame, and every draw first requires
  the two buffers to agree (stage `frame`), so a missed case fails closed
  instead of drawing dark.
- `Resize` leaves the scene extent equal to the extent it was given, so the
  view rectangle the owner passes matches the renderer's scene. A size larger
  than the device's largest texture is refused.
- The projection is `CG_UpdateFov`'s (fixed vertical field of view; horizontal
  widens with aspect), not the probe's fixed 65-degree horizontal. The two agree
  at 4:3. `SetAspect` should get `vidConfig.aspectRatioWindow` after a resize,
  as the cgame reads it.
- A view takes a few draws before the static model cache settles; comparisons
  use settled frames.
- An engine error (`Com_Error`, `Sys_Error`, `R_FatalInitError` when
  `IDirect3DDevice9::Reset` fails) ends the process, as it does in the client.
  Everything the adapter checks itself is a refusal with a reason.
- `r_vc_makelog` must be off: `R_LoadWorld` would start a light visibility log
  only `R_Shutdown` saves.

`FillRefdef` writes only the camera's fields (rectangle, both half-FOV tangents,
origin, axis, `zNear`, `time`) and is a template, so this layer never includes a
renderer or D3D9 header and never puts a native DXVK type into an engine
structure. The owner never asks for a view at an empty extent, so
`R_RenderScene`'s positive width/height/tangent assertions hold.

## The replay

`KISAK_FREE_CAMERA_MAP=<map>` runs `KisakCOD-world-draw-probe` as the free-camera
replay (`src/linux/tools/free_camera_replay.cpp`). After the probe's renderer
start it builds an `EngineSceneRenderer` at the window's 640x480, puts a
`FrameOwner` over it, and acts only as a window host would: keys, mouse counts,
extents and frames. Every frame is drawn at scene time 0 and read back into
memory to be measured; nothing is saved.

1. Load the map, choose its cameras as the probe does (A: the intermission
   camera, B: a player spawn), then load it again with camera A's pose.
2. Path A, as `frame_owner_test.cpp`'s reference traversal: `w` for 60 frames of
   16 to 17 ms, an 818-count mouse turn left, `w` with shift for 10 frames of
   100 ms, then space for 5 frames of 100 ms. The view is settled (the probe's
   rule) at the start and after each leg: `A.start`, `A.forward`, `A.turn`,
   `A.fast`, `A.rise`.
3. At `A.rise`, four interruptions, each followed by a settled view that must
   be the same picture as `A.rise` (same digest):
   - resize: the window and the owner go to 800x600 (drawn and settled there,
     `A.rise@800x600`), then back to 640x480;
   - minimize: an empty extent for three frames, which must draw nothing, then
     640x480 again, which needs no reset;
   - device reset: `RequestReset`, so the owner's next frame resets the device;
   - reload: the map is unloaded and loaded again; the camera must not move.
4. Path B from camera B: the arrow-key square, four sides of `w` for 5 frames of
   200 ms and a 642 ms turn left. Settled at `B.start` and `B.square`.
5. The owner's shutdown: the map's unload, then `R_Shutdown(1)`.

Rules, any of which fails the run:

- `A.start` and `B.start`, the map's own cameras, pass the probe's frame and
  draw count rules;
- every checkpoint settles within 64 draws and is drawn at its extent;
- each path changes the picture (start and end digests differ);
- each interruption comes back to the same settled picture;
- the counts agree: four loads and four unloads, two resizes, one reset, an
  empty frame after `R_Init` and after each reset, as many draws by the owner as
  by the renderer;
- after the shutdown no device, Direct3D interface, registration, map or zone is
  left.

Every frame of a leg must be drawn; a frame the owner reports as anything else
stops the run with its stage. The run prints `free_camera_leg` (with how many of
the leg's frames had the eye in solid and drew no world call, and its frame of
fewest colours: index, pose, visibility cell and contents, followed by that
frame's `world_draw_frame` line labelled `<leg>.fewest_colors`),
`free_camera_checkpoint` (with the pose and digest), `free_camera_interruption`
(`same=yes|no`), the settle frames as `world_draw_frame` lines,
`free_camera_counters`, `free_camera_shutdown` and finally
`free_camera_replay=passed|failed`, and writes `free-camera-replay.json` (cameras,
checkpoints with every settle frame's measurements, legs, interruptions,
counters, problems; no pixels).

A leg frame that drew no world call is looked at again after path B, as a
report and never as a rule (up to four per leg, drawn outside the owner's
counters). Before the first, the portal graph: each cell's bounds and the cells
its portals lead to (`free_camera_visibility_cell_links`), any portal with no
portal back (`free_camera_visibility_one_way_portal`) and the totals
(`free_camera_visibility_portals`). For each frame, `free_camera_visibility`
lines draw its pose as it was, with single-cell visibility, without
pretessellation, without the static model cache and without shadows, then moved
by 1 to 32 units and turned by 1 to 90 degrees, each with the eye's cell, the
cells marked visible, the draw counts and the picture's colours;
`free_camera_visibility_cell` and `free_camera_visibility_portal` give the
eye's cell and every portal's distance from the eye, and
`free_camera_visibility_bounds_holding` the cells whose bounds hold it.
`free_camera_visibility_ray` traces, apart from the visibility walk, the view's
centre and four corners through every world triangle and through the collision
world (`MASK_SOLID`): the nearest triangle, whether its stored normals face the
eye, the nearest one that does, the cells whose surface lists hold each, and the
first collision face with its material. The rays run from the pose as it was and
from six of the moved poses.

Run it as the [world draw probe](linux-world-draw-probe.md#running-it) runs,
with `KISAK_FREE_CAMERA_MAP=mp_crash` in place of `KISAK_WORLD_DRAW_MAP`, a new
0700 output directory, and the same staging tree. The window is resized during
the run; a window manager that refuses the size changes only the
`window_resized` field, because the back buffer follows the owner's extent.

## Window input

The renderer's window is the one `R_CreateWindow` makes through
`src/linux/client/window/win32_window_sdl3.cpp`; DXVK presents to it, and nothing
else pumps its events (the world draw probe and the adapter never do).
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
  `FrameDeadline` or a clock; the owner caps a step at 200 ms). The aspect is the
  owner's default, width/height, unless the caller passes
  `vidConfig.aspectRatioWindow` through `FrameOwner::SetAspect`, as the
  [window driver](#the-window-driver) does after every resize.
- Quitting (Escape, window close, `RequestQuit`) only stops the loop. The order on
  the way out is: destroy `FreeCameraWindow` (capture and keys released, window
  left alone), then `FrameOwner::Shutdown` (unload, then the renderer, which may
  destroy the window).

There is no console or menu in this loop, so no key catcher decides who owns a
key. When one is added, keys must go through `ClientInputHost` and its catchers
before they reach the camera.

## The window driver

`KISAK_FREE_CAMERA_INPUT` turns the free-camera mode of
`KisakCOD-world-draw-probe` from the replay into the window driver
(`src/linux/tools/free_camera_window_driver.cpp`). It starts the renderer as the
replay does, loads the map at camera A, then adopts the renderer's window
(`EngineSceneRenderer::Window()`) with `FreeCameraWindow`, and every frame is a
`Tick` of that window. On the way out `FreeCameraWindow` is destroyed first (the
window must still exist afterwards), then the owner shuts down, which unloads
the map and runs `R_Shutdown(1)`; after that the window, the device, the map
and every zone must be gone.

The engine picks the aspect for a size itself (`R_StoreWindowSettings`: with
`r_aspectRatio` auto a windowed size snaps to 4:3, 16:10 or 16:9). The driver
gives the owner that aspect after every resize from inside the renderer's
`Resize`, which the owner calls at the start of a frame before it builds the
view, so the first frame at a new size is already drawn with the engine's
aspect.

`KISAK_FREE_CAMERA_INPUT=window` is the live camera. Click in the window to
capture the pointer, then `w`/`a`/`s`/`d` to move, space and `c` for up and
down, shift for fast, the arrow keys to turn; Escape releases the pointer and
Escape again (or closing the window) quits. Frames are timed from a monotonic
clock and drawn at the elapsed time without reading back. An optional
`KISAK_FREE_CAMERA_SECONDS` (1 to 3600) quits by itself. It prints
`free_camera_window_controls` at the start and `free_camera_window_live` (how it
ended, frames by outcome, frames per second) at the end.

`KISAK_FREE_CAMERA_INPUT=script` drives the same window with SDL events it
pushes into the window's own queue (`SDL_PushEvent`), so every key, click and
pointer motion goes through SDL, `WindowHost` and `FreeCameraWindow` as a
desktop's would. Window size and minimize go through the window system itself
(`SDL_SetWindowSize`, `SDL_MinimizeWindow`, `SDL_RestoreWindow`). A second
`FrameOwner`, over a renderer that draws nothing, is given the same input
directly, as the replay gives it, and the camera must be at exactly the same pose
at every checkpoint. Frames are drawn at scene time 0 and read back, and each
checkpoint settles by the world draw probe's rule.

1. Focus (pushed: a desktop may not focus a new window), settle `W.start`, then a
   left click, which must capture the pointer.
2. Path A of the replay as events: `w` for 60 frames of 16 to 17 ms, an 818-count
   pointer motion, `w` with left shift for 10 frames of 100 ms, space for 5 frames
   of 100 ms; settled at `A.forward`, `A.turn`, `A.fast`, `A.rise`.
3. Resize: the window is set to 16:9 at its height (854x480 from 640x480), the
   back buffer must follow its pixel size and the owner the engine's new aspect
   (`A.rise.wide`); back at its size the picture must be `A.rise`'s again.
4. Minimize: no frame may draw while the window is minimized; after the restore
   the capture must come back and the picture be `A.rise`'s. Wayland neither tells
   a client that it was minimized nor lets it restore itself, so there this step
   is recorded as not taken.
5. Focus loss while `w` is held: the camera stops and the capture is released;
   focus back re-applies the capture and presses nothing.
6. Escape releases the pointer, Escape again quits, and no frame runs after.

A window manager that refuses the 16:9 size is recorded the same way, as a step
not taken; each step prints `free_camera_window_step name=... exercised=yes|no`.
Any other difference fails the run: a checkpoint that does not settle, a pose
that differs from the directly fed owner's, a back buffer that is not the
window's pixel size, a picture that does not come back, a frame drawn while
minimized or after quitting, or a capture that does not follow. The counts must
agree as for the replay, and after the shutdown the window must be destroyed and
no device, map or zone left. Input the script did not push is counted, not
refused: every pushed event carries a marker device id, an SDL event watch
counts key, pointer and wheel events for the window without it, and focus losses
that came from the desktop are counted too. A desktop may send pointer motion as
a window appears under the pointer, which the owner ignores while nothing is
captured; real input that does reach the camera shows up as a pose difference,
and the count is then named beside it.

Both write `free-camera-window.json` (the camera, the counts, and for the script
its checkpoints with poses and digests and its steps; no pixels) and end with
`free_camera_window_run=passed|failed`. Run them like the replay, adding
`KISAK_FREE_CAMERA_INPUT`. The scripted run moves, resizes, minimizes and captures
the pointer of a window on the desktop for a few seconds; nobody should use the
mouse or keyboard meanwhile, because input that reaches the camera fails the run.

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

Adapter and replay, in the cloud sandbox only (no GPU, no game data): the
target builds in Debug and RelWithDebInfo against DXVK Native with
`-Wall -Wextra -Werror`, and `scripts/tests/test_world_draw_probe.py` runs the
replay against the stand-in D3D9 library: it must stop at its own
`stage=direct3d` before `R_Init`, and refuse an unknown map, both map variables
at once and a show time first, writing nothing.

Adapter and replay on a GPU (David's laptop: Intel Raptor Lake-U graphics, Mesa
ANV 25.0.7, Debian 13 under GNOME Wayland, DXVK 2.7.1 and SDL 3.2.10 for i386,
Clang 19.1.7; the owner's installed `mp_crash`; local runs, not CI evidence), on
head 39be5b4 in RelWithDebInfo and Debug, and again on c6e8164 with the leg
reports below (the same digests and counts): every run passed with no problem. All
twelve checkpoints settled (within 2 to 9 draws); the resize, minimize, device
reset and reload each came back to `A.rise`'s digest; the counts were 4 loads, 4
unloads, 2 resizes, 1 reset, 4 empty frames and 153 draws by both the owner and
the renderer, with 3 minimized frames that drew nothing; after the shutdown no
device, map or zone was left. The two builds printed the same checkpoint and
frame lines and wrote the same report apart from times. The run took 10.8 s in
RelWithDebInfo and 21.7 s in Debug, both including the map's four loads. DXVK
printed `D3D9Cursor::ResetCursor: Not supported on current platform` at each of
the three device resets; nothing else was a warning or an error.

At the same eye and angles, the replay's settled `A.start` and `B.start` are not
bit for bit the probe's views A and B (a few pixels apart: for A, 63,428
colours against 63,434 and the same mean luma). The only refdef field that differs is the
vertical tangent: the camera computes it as `CG_UpdateFov` does (0.47780272),
the probe as width over height (0.4778027), one float step apart.

Two frames in motion are flat because path B flies into a closed pocket whose
walls are drawn only on their far sides. Frame 22, the end of the
square's third side, at (-155.6, 205.8, 222) facing yaw 179.76, and frame 29 at
(-156.32, 91.8, 222) facing yaw -90.36 are each a single black colour: the
front end emitted at most one lit surface (a static model) and the backend
issued no world draw call. The visibility report (run N=44 in Debug on 4a0049c;
N=43 on 2b2aaf3 gave the same draws) shows why:

- Both eyes are in visibility cell 1, the open ground to their east, and only
  cell 1 is marked visible. Single-cell visibility, no pretessellation, no
  static model cache and no shadows all draw the same nothing.
- Every ray from both eyes, centre and corners, first meets a collision face of
  the `caulk` material, 16 to 64 units away and facing the eye. 12 to 34 units
  further it meets the back of a drawn wall (`wc/me_cinderblock_wall2`, once
  `wc/me_metal_rust_02`) listed by cell 10, to the west, or cell 21, to the
  south. Caulk faces are not part of the drawn world, back faces are culled,
  and cell 1 has no portal to cells 10 or 21, so nothing in view can be drawn.
- The pocket is closed on the east the same way: a caulk face with the back of
  a roll-up door (`wc/me_door_rollup1`) behind it. Turned by 90 degrees towards
  cell 1's own surfaces, each eye draws a full picture (4 to 8 cells). Moved 32
  units south through the wall, frame 29's eye is in cell 21 and draws 7 cells.
- The portal graph is whole: 23 cells and 180 portals, each with a portal back.

The eye is not in solid (its contents are `CONTENTS_STRUCTURAL`, as at path A's
normal frames): the free camera has no collision and flew through a wall into
the air behind it. The leg frames whose eye is in solid (two in `A.fast`, four
in `A.rise`) drew normally, and every checkpoint settled on a full picture.

Window driver, in the cloud sandbox only: it builds into the same target in
Debug and RelWithDebInfo with `-Wall -Wextra -Werror`, and
`test_world_draw_probe.py` runs both inputs against the stand-in library (each
must stop at `free_camera_window stage=direct3d` before `R_Init` and before the
window host exists) and refuses an unknown input, an input in the draw mode, a
time limit outside 1 to 3600 seconds and a time limit on the script, before any
device work. That SDL 3.2.10 keeps the marker device id on pushed events and
that the event watch sees both kinds was checked with a small program on the
dummy driver.

Window driver on a GPU, on the same laptop (local runs, not CI evidence).
`KISAK_FREE_CAMERA_INPUT=script` from b0209da in RelWithDebInfo (run N=40,
2026-11-02) passed with no problem in 7.1 s: SDL chose its Wayland driver, the
adopted window started unfocused at 640x480, and every checkpoint (`W.start`,
`A.forward`, `A.turn`, `A.fast`, `A.rise`, `A.rise.wide`, `A.rise.resized`,
`A.focus`) settled with the camera at exactly the directly fed owner's pose. The
path A digests are the replay's own on the same machine. The 16:9 resize reached
the back buffer at 854x480, the engine's aspect went from 1.3333 to 1.7778 with
it, and back at 640x480 the picture was `A.rise`'s again. Focus loss and the two
Escapes behaved as above. The minimize step was not taken (Wayland). The event
watch counted two pointer or key events the script did not push, with nobody at
the laptop; they reached no camera (every pose matched). After the shutdown the
window was destroyed and no device or zone was left. DXVK logged
`D3D9Cursor::ResetCursor: Not supported on current platform` once per device
reset, as in the replay's runs.

The same script from c54596a (the same driver) in Debug with
`SDL_VIDEODRIVER=x11` (GNOME's XWayland, run N=41) passed with no problem in
14.3 s. Every checkpoint's digest was N=40's, and the minimize step ran: the
window manager reported the window minimized, five frames were skipped without a
draw, and after the restore the capture came back and the settled picture
(`A.rise.restored`) was `A.rise`'s. The desktop took focus once (the minimize),
no input came from outside the script, and no window was left after the
shutdown.

The live camera, `KISAK_FREE_CAMERA_INPUT=window` with a 900 s limit, from the
same RelWithDebInfo build under Wayland (run N=42): David flew `mp_crash` for
70 s and quit with Escape. The run passed with no problem: 3,443 frames, all
drawn (49 per second), none minimized or with a lost device, one capture
request; the shutdown released the device and every zone and destroyed the
window. DXVK printed no warning or error.

## What is not verified

- The replay has run on one GPU and one map. Its interruptions are forced: DXVK
  never reports a windowed device lost, so a real loss, and a reset it makes
  necessary, has not been seen.
- Nothing is compared with a captured game frame.
- That the game also draws nothing inside path B's pocket follows from the
  map's faces and the visibility walk's rules; it has not been seen on Windows.
- The window input has met a real desktop through the window driver: its
  script (Wayland in RelWithDebInfo, X11 in Debug) and one 70 s live flight
  (Wayland), with the window `R_CreateWindow` made. The live flight was not
  resized or minimized, and in the script focus changes other than the
  minimize's were the script's own.

## Acceptance on a GPU

Run on named hardware with a private map, keeping screenshots and exports out of
the repository. The replay covers items 1 and 2, item 3 without a map switch,
and the shutdown order of item 4; it passed on David's laptop (see What is
verified). The window driver's script repeats path A, the resize and the
minimize through the renderer's own window, with quitting from the keyboard; it
passed there too.

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
