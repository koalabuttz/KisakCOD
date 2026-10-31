# Native Linux window/input checkpoint — 2026-10-31

Branch: `dot/client-window-2026-10-31`, based on client lifecycle checkpoint
`43550e60`. Server reliability history remains independently reviewable.

## Delivered behavior

An independently opt-in SDL2 backend now creates, presents, and closes a real
Linux window and handles keyboard/text/mouse, resize, focus and relative capture.
The runnable `KisakCOD-client-window` shell exercises that backend and the actual
engine event queue. It reports `game=false`: its consumer records events instead
of running the game. Build and run commands are in [linux-client.md](linux-client.md).

The existing Linux event ring was extracted from `main.cpp` into `sys_events.cpp`.
Its normal drop-oldest overflow and payload ownership semantics are preserved;
console polling remains in main. The input adapter adds a narrow queue-compaction
operation that discards stale key/text while retaining console payloads in order.
Focus loss/minimize/quit also require a consumer reset callback, so an evicted
key-up cannot leave already-consumed keys stuck. Tests reproduce that exact
ring-overflow sequence, and preserve multiple owned console payloads across a
full wrapped ring without duplicate frees.

The backend aggregates left/right modifiers, bounds text buffers, validates UTF-8
before emitting printable Latin-1 for the engine's existing byte field interface,
filters foreign window IDs, calibrates SDL event timestamps into the engine clock,
and bounds each pump. Relative capture releases on blur/quit/destruction and
reacquires on focus gain if requested. Unsupported backend capture fails visibly.
The adapter signatures match actual `CL_MouseEvent` and `Key_ClearStates`, but the
shell deliberately supplies a reporting consumer, not those game services.

SDL owns a process-wide event queue: a second simultaneous host is rejected.
Callbacks must not throw; their sink must outlive the host. SDL subsystem and
text-input ownership are balanced. The shell requests software framebuffer
presentation, presents once before announcing readiness, and has explicit bounded
run options. It does not select a game rendering backend.

## Validation and evidence

All final checks passed in Debug and RelWithDebInfo. Evidence lives at
`/workspace/kisakcod-client-window-results/` in this workspace.

| Check | Final result | Evidence |
| --- | --- | --- |
| Optional window checker | 42/42 each configuration | `debug.log`, `release.log` |
| Real SDL event/engine queue suite | 34/34 each, plus independent Debug rerun | checker logs, `window-independent.log` |
| Existing client-core aggregate | 438/438 each | `core-debug.log`, `core-release.log` |
| Core checker driver tests | 8/8 | `driver.log` |
| Dedicated default-OFF build and supported aggregate | builds passed, 12/12 each | `server-*.log` |
| Focused Linux runtime including new queue TU syntax check | passed each | `runtime-final-*.log` |
| Actual X11 shell, ordinary mode | passed both configurations | `x11-debug-final.log`, `x11-release-final.log` |
| Actual X11 shell, relative mode | passed both configurations | `x11-debug-relative.log`, `x11-release-relative.log` |

The SDL suite uses official `SDL_PushEvent` with the dummy video driver, production
backend/adapter/queue/locks, and an explicit consumer recorder. The separate X11
script launches owned Xvfb and shell processes, injects OS events with XTest via
xdotool, and sends `WM_DELETE_WINDOW` through Xlib. It verifies resize, keyboard,
text, motion, buttons/wheel, held-key focus reset, fresh key input after regain,
and clean close with zero held keys. Relative mode additionally verifies active
capture, release on blur, reacquisition and release on close. These are real X11
window/event-path tests with synthetic OS input, not physical-device tests.

An initial X11 run exposed a readiness race: SDL's first window-surface creation
could replace the native XID after the shell's earlier creation announcement.
The shell now presents before announcing `window=ready`; all final X11 runs use
that barrier. Test fixtures also account for SDL assigning pushed-event timestamps
and its poll sentinel when an externally owned event subsystem persists.

Independent read-only review found no remaining blocking issues. Its overflow
recovery concern prompted the explicit consumer reset contract; the final review
also covered capture lifetime, clock wrap, text bounds and ownership. Dedicated
compile databases and ELF dependencies contain no SDL/window backend or SDK flags
with the option OFF. Existing client core still defaults OFF as well.

## Toolchain and dependency provenance

The environment prohibits direct native ELF32 execution, so existing Clang 19
multilib builds run under the existing QEMU i386 adapter. This is executable
32-bit code, not a compile-only result; direct native and 32-bit sanitizer
validation remain unverified. The known sanitizer startup hang was not retried.

SDL2 2.32.4 i386 headers/runtime and host Xvfb/xdotool were obtained from official
Debian trixie packages using signed InRelease metadata and the Debian archive
keyring. Packages were downloaded/extracted under `/workspace/native-tools`,
without installation scripts or system package changes. No SDK is vendored into
this repository. `dependency-provenance/` contains package filenames, SHA256
manifests and verification logs. The relocated SDK requires its architecture
include directory and linker search paths; the local QEMU environment supplies
its extracted SDL library path. No proprietary assets were obtained or needed.

## Remaining boundary and next step

This is a usable platform shell, not a playable Linux CoD4 client. It neither
initializes `CL_Frame`/client UI consumption nor joins the existing engine-network
diagnostic into one game host. D3D9 game rendering, audio, cgame/asset startup and
full game-loop integration remain unfinished. SDL dummy/X11 success establishes
neither a game GPU renderer nor hardware-input coverage. Linux authentication
still fails closed: the available SDK headers/Windows DLL do not supply a Linux
`libsteam_api.so` runtime/provider. No login, synthetic public authentication,
credentials or admission behavior was changed.

The next bounded architecture step is an engine host that drains this real queue
into actual client key/character/mouse/reset consumers and services engine events
on the same lifecycle as the existing NET diagnostic, preserving the unavailable
provider boundary. Renderer/platform ownership should remain modular while that
integration acquires production runtime tests. Actual gameplay and authenticated
joining require separate dependencies and validation; no current test claims them.
