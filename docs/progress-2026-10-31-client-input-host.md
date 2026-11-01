# Native Linux input-consumer checkpoint — 2026-10-31

Branch `dot/client-input-host-2026-10-31`, based on `8a4503f9`.

## Actual integration

`KisakCOD-client-input` is a runnable, disconnected SDL input host. SDL events pass
through the production event ring into actual `CL_KeyEvent`, `CL_CharEvent`,
`CL_MouseEvent` and `Key_ClearStates`. Text edits the actual engine field with
`Field_KeyDownEvent`/`Field_CharEvent`; key repeats and held state come from
`playerKeys`, not a parallel recorder. Trace output reports editor text/cursor and
real held-key count. The plain surface does not draw the editor or a game UI.

The new `CL_InputServices` interface separates external console/UI/DevGui/script,
clipboard-adjacent and text-layout services from input state/edit semantics.
The normal game provider forwards the existing calls unchanged. It is selected
through normal shared source lists, for multiplayer and singleplayer. The new host
links its own scoped, noncopyable provider; no nullable runtime override modifies
a running game's services. Access outside host lifetime is an explicit error.
The full production key TU is compiled, not a copied editor implementation.

The standalone console-key service supports field editing only. Enter, binding
commands, autocomplete, game UI/script/DevGui and other unavailable operations
report that limitation; they do not pretend to execute or authenticate. Clipboard
paste explicitly reports unavailable UTF-8/legacy-byte conversion. Layout uses an
explicit eight-pixel terminal-cell policy, not fake engine font metrics. Tiny
windows clamp to one usable cell. This host does not run full `Console_Key`
autocomplete/history/command execution. The default game adapter still does.

`Field_Clear` moved unchanged from `qcommon/common.cpp` to `qcommon/field.cpp`,
listed in the shared QCOMMON sources. Linking the old full common TU solely for
this helper pulled unavailable OpenAL headers; the extraction avoids weakening or
stubbing audio services.

Focus loss/quit first discard stale queued input, then run actual `Key_ClearStates`,
`CL_ClearKeys`, and clear both accumulated mouse buffers. This covers held keys,
button active/pulse state and unconsumed mouse motion. The host creates no command
backlog: binding command execution is explicitly unavailable. Each host input tick
samples and clears actual accumulated mouse deltas; it is not a game frame.
Popped event payloads are owned until dispatch completes, including error paths.

A scoped SIGINT/SIGTERM helper is installed after SDL initialization and restores
prior handlers on teardown. An always-lock-free atomic records the first request
across helper threads; handlers call no SDL/engine/logging/allocation functions.
The loop observes the request, reports it and uses normal window/input shutdown.

## Build and run

The existing default-OFF `KISAK_BUILD_LINUX_WINDOW_HOST` option adds the new host,
`KisakCOD-client-input-test`, and `KisakCOD-shutdown-signal-test` alongside the
previous shell/test. SDL2 and the 32-bit toolchain requirements are unchanged.

```sh
cmake --build build-linux/window --target KisakCOD-client-input KisakCOD-client-input-test KisakCOD-shutdown-signal-test -j4
./build-linux/window/bin/KisakCOD-client-input --trace --relative --run-ms 10000
SDL_VIDEODRIVER=dummy ./build-linux/window/bin/KisakCOD-client-input-test
python3 scripts/tests/test_linux_window.py --build-dir build-linux/window
```

Use `--frame` to request a game frame: it exits 5 with `frame=unavailable` and the
missing production services, before creating a window. This is an explicit
boundary, not a call to `CL_Frame` with `isRunning=false` advertised as progress.
Close/Escape and SIGINT/SIGTERM shut down cleanly. Other command-line options match
the previous window shell. For external tests, use
`test_linux_window_x11.py --engine-input --relative` with the new binary and the
same Xvfb/xdotool/runner arguments as the window checkpoint.

## Validation

Final evidence is under `/workspace/kisakcod-client-input-host-results/`.

| Check | Result | Evidence |
| --- | --- | --- |
| Window/input checker | 60/60 Debug and RelWithDebInfo | `window-*-final.log` |
| Actual client input lifecycle | 36/36 each | window checker logs |
| Prior SDL window/queue runtime | 34/34 each | window checker logs |
| Client-core aggregate | 441/441 each | `core-*-final.log` |
| Checker driver | 8/8 | `driver.log` |
| Signal ownership/concurrent requests/restoration | passed native64 and QEMU32; both configured builds | `shutdown-signal-*.log`, window checker |
| Real X11 input and shutdown | passed both configurations | `x11-debug.log`, `x11-release.log` |
| Dedicated default-OFF build and supported aggregate | builds passed, 12/12 each | `server-build-*-final.log`, `server-aggregate-*.log` |

Input tests run SDL events into actual production consumers, exercise repeat,
insertion/backspace/navigation, tiny resize, mouse accumulation/consumption, actual
ring overflow and focus recovery, unavailable commands/frame requests, quit, and
two complete scoped-host lifetimes. A labelled command-argument fixture calls
actual `IN_Attack_Down`/`IN_Attack_Up` to leave active and unconsumed pulse state
before reset. It does not claim a real command executor or simulated active game.

X11 tests inject XTest OS events through xdotool into private Xvfb windows. They
verify real editor changes, resize with a held key, focus recovery/fresh input,
relative-capture lifetime, and separate WM-close/Escape/SIGTERM/SIGINT lifecycle
runs with zero actual held keys and no orphan windows. The previous shell mode
also remains compatible. These are synthetic OS-input tests, not hardware input.

Independent review covered default forwarding parity, provider lifetime, reset,
mouse consumption and signal safety. Review led to cross-thread-safe signal
recording, queued-payload error cleanup and an unconsumed button-pulse regression.
The default adapter and full key source compile for MP and SP; no Windows runtime
or complete SP client build is claimed. Dedicated build evidence confirms no SDL
sources/SDK flags/ELF dependency with options OFF, and includes the new shared
field TU. The dedicated aggregate explicitly skips `handshake_auth`,
`server_test_clients`, and `udp` under the existing direct-ELF32, sanitizer startup
and shell-execution limitations. Its 12 supported baseline checks do not claim
the separate server-reliability branch's full 15-check result. Server checkpoint
`b5e3d408` and `/workspace/kisakcod-refill-results/server-cumulative.patch` remain
independent of this client history.

An initial core-check invocation changed the compiler alias from `clang++-19` to
`clang++`, causing CMake to reset cached options. The checker correctly rejected
the lost opt-in. Fresh configurations with explicit matching compiler paths were
then used for final aggregate runs; initial failure logs remain as evidence.

## Remaining boundaries

No game rendering/audio, command execution, cgame startup, authenticated joining,
or gameplay is provided. `CL_Frame` additionally needs initialized hunk state,
DevGui/voice, download, cgame timing and authentication services. The NET discovery
program and input host remain separate executable lifecycles. Linux Steam runtime
`libsteam_api.so` is unavailable in authorized dependencies; authentication remains
fail-closed. No fake connected/active state, credentials, login, or public auth
bypass was added.

All project binaries execute as ELF32 through the existing QEMU adapter because
direct native ELF32 execution is unavailable. Native hardware/GPU and 32-bit
sanitizer validation remain unverified. Dependencies retain the official extracted
Debian SDL/Xvfb/xdotool provenance recorded in the window checkpoint; no new
proprietary assets or system package changes were needed.

Next, integrate production command registration/execution and networking into an
explicit engine startup owner, preserving unsupported subsystem boundaries and
reset ordering. A complete game frame should only be enabled when its real
services initialize successfully; a renderer/backend milestone remains separate.
