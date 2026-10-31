# Production Linux input startup prerequisite — 2026-10-31

Branch `dot/client-input-startup-2026-10-31`, based on `6f4d13bd`.
The worktree retains its original path `/workspace/KisakCOD-client-packet-host`.

## Delivered independent milestone

The existing standalone input host now calls unchanged production `CL_InitInput`
and `CL_ShutdownInput`. It no longer manually registers four IN handlers. The full
75-command movement/action/stance/ADS/talk set and its three input dvars are now
initialized by the same code used by `CL_InitOnceForAllClients`.

The host supplies the normal `cl_freelook=true` default, owns the `cl_nodelta`
pointer populated by production initialization, and clears input dvar pointers
before runtime teardown. The shared runtime updates real `com_frameTime` from its
monotonic clock before command execution and clears it at shutdown. Actual input
reset, command-buffer cancellation, binding cleanup and repeated owner lifecycle
remain in place. No alternate input implementation or successful game-service
stub was introduced.

The focused test verifies exact registration and defaults, real command execution
for action/talk/stance state, focus-reset cancellation, input-only shutdown while
host commands remain alive, reinitialization and repeated complete ownership.
Public state stays disconnected with renderer, game UI and `isRunning` unset.

See [the frame dependency map](linux-client-frame-dependencies.md) for concrete
remaining memory, voice/audio, DevGui, renderer, cgame, download and authentication
requirements. `CL_Frame` and its production ordering are unchanged. This is a
real startup prerequisite, not a claim that a game frame ran.

## Build and evidence

The default-OFF `KISAK_BUILD_LINUX_WINDOW_HOST` option now also adds
`KisakCOD-client-input-startup-test`. It uses the same SDL/multilib setup as the
existing input frontend.

```sh
cmake --build build-linux/window --target KisakCOD-client-input KisakCOD-client-input-startup-test -j4
SDL_VIDEODRIVER=dummy ./build-linux/window/bin/KisakCOD-client-input-startup-test
python3 scripts/tests/test_linux_window.py --build-dir build-linux/window
```

Evidence directory: `/workspace/kisakcod-client-input-startup-results/`.
Final Debug and RelWithDebInfo results:

| Check | Result | Evidence |
| --- | --- | --- |
| Window/runtime aggregate | 83/83 each | `window-*-final.log` |
| New production input startup | 23/23 each | aggregate logs |
| Existing input/Cbuf lifecycle | 40/40 each | aggregate logs |
| Command and shared runtime regressions | 28/28 and 42/42 each | aggregate logs |
| Client-core aggregate | 472/472 each | `core-*-final.log` |
| Checker driver | 8/8 | `driver.log` |
| Existing unchanged X11 regression | passed each | `x11-debug.log`, `x11-release.log` |

Independent review of the startup-only change found no blocking issues. Existing
X11 input/command/shutdown regressions were rerun unchanged; no new packet test
scope was introduced.

The initial independent link/runtime probe used unchanged `CL_InitInput` and
identified only the actual input dvar and clock globals needed by this host.
The final targets execute production code as ELF32 through the existing QEMU
adapter. Direct native hardware, GPU and sanitizer validation remain unverified.
Existing renderer/authentication/gameplay limitations remain unchanged.

## Paused packet work — not delivered

The proposed production packet extraction/hardening worker was blocked before
execution. Its returned error was:

> Agent errored: This content was flagged for possible cybersecurity risk. If this seems wrong, try rephrasing your request. Otherwise, confirm that the access_programs.cyber parameter is set to the appropriate tier, and note that some cybersecurity requests are still limited, even when Daybreak is on.

The [recorded blocker](/workspace/kisakcod-client-input-startup-results/packet-work-blocker.json)
contains the exact returned error and outcome. The target was proposed
`CL_PacketEvent` extraction and receive regression/fix work
in this worktree. The worker executed no files or tests. No retry or alternate
execution route was attempted. The parent explicitly paused that scope and its
dependent central packet-dispatch integration. No packet implementation, new
packet tests, or changed receive path are included in this checkpoint. Existing
regressions were retained for the independent startup work.

The original broader milestone therefore remains incomplete. Continuing packet
work requires resolution of that block; the existing authorization was not
reinterpreted as new evidence. Full game frame/render/audio startup and a supported
Linux authentication provider also remain unfinished. No pushes, merges, login,
credentials or proprietary-asset acquisition occurred.


The server/client series remain independent. A disposable combined checkout is
being checked separately; its evolving report is
`/workspace/kisakcod-integration-check-results/REPORT.md`. This checkpoint does not
claim pending combined-run results or merge those histories.
