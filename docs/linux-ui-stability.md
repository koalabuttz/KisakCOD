# Native UI lifecycle stability

This checkpoint consolidates the existing local control host; it does not add
menu profiles, game-frame execution, authentication, or packet handling.

Two menu-owner faults were reproduced with the real privately prepared assets.
The old stream-based telemetry allocated during state reporting. An injected
allocation failure was swallowed by the stream and emitted a truncated state
line, also replacing its change-detection cache. Telemetry now compares a fixed
state record and prints fixed fields without C++ allocation. The genuine steady
frame is likewise checked for C++ allocations after initialization.

A test-only wrapper around `SDL_UpdateWindowSurface` returns one explicit
backend failure, otherwise calling real SDL. Previously this left a pending
renderer frame, so the next owner presentation failed before it could record.
The owner now cancels failed recording/presentation work through an explicit
`CpuRenderer::CancelFrame` boundary and propagates the error. Cancellation
preserves resource handles and current CPU pixels; it does not claim to roll
back an already-copied SDL surface or a displayed frame. The presented counter
advances only on success. A later caller can retry with the same owner. The
executable still reports an unhandled presentation error and exits; it does not
silently retry backend failures.

A physical resize allocation failure already retained the old renderer
resource generation and recovered on the next call. That behavior is tested
and preserved. No speculative input-constructor fix was made: inspection found
that its current post-runtime scalar dvars and static command nodes do not
allocate on the hypothesized failure path.

The genuine owner fixture now sends repeated keys through `CL_KeyEvent`, checks
that repeat does not toggle yes/no controls twice, releases held input on focus
loss, and checks actual UI-catcher removal after Escape. Nondefault settings
survive close/reopen and menu-owner recreation within the same input runtime.
A fresh input runtime returns to registered defaults; the fixture does not
write or load a user configuration file.

## Next renderer prerequisite

`R_Init` and `CL_InitRenderer` still require the D3D9 device, backend assets and
world/registration services. `R_BeginFrame` additionally assumes registered
renderer, skin-cache and debug/world state. Setting their readiness flags would
not establish a native backend.

A bounded independent next step is sharing the device-free command-source
startup routines `R_InitCmdBufSourceState`, `R_SetViewportStruct/Values`, and
`R_Set2D`/`R_CmdBufSet2D`. An owned native backend state could then use the real
constant-version initialization and half-pixel projection during initialization
and resize. Exact matrix/viewport tests and current presentation readback would
precede a real GPU vertex path. This is a proposal, not an implemented renderer
or game-frame claim.

The concrete observable integration candidate is the existing
`CL_SetViewport -> R_AddCmdSetViewportValues -> RC_SET_VIEWPORT` command path.
The current CPU consumer rejects that command. A bounded implementation would
consume the actual command and shared viewport state to position and clip
subviewport draws, with exact pixel readback and unchanged full-window output.
It should use the production 2D projection where it affects coordinates, rather
than merely retaining unused matrices. It must prevalidate the entire command
list, reset full-window viewport state on resize, and keep unsupported 3D/device
operations explicit. The current checkpoint implements none of this proposal.

## Window and audio ownership checks

Window acquisition now unwinds its SDL window, subsystem reference and singleton
publication when construction fails. Destruction is explicitly nonthrowing and
performs best-effort callback cleanup before releasing SDL resources; callback
failures are reported instead of terminating midway through cleanup. The old
constructor leaked ownership in a throwing-clock fixture, and the old destructor
aborted in a throwing-quit fixture. Ordinary event dispatch still propagates
errors to the caller.

Audio resource tests exercise real dummy-device acquisition failure, allocation
failure while loading sound data, bounded overlapping voices and repeated close
with pending samples. These checks did not identify a production audio ownership
defect, so no speculative provider rewrite is included. SDL disk-backend output
can establish exact emitted sample bytes; it does not establish audible playback
on a physical device.
