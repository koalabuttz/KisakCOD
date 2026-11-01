# Shared Linux command/network runtime — 2026-10-31

Branch `dot/client-command-host-2026-10-31`, based on `c5021253`.

## Delivered production behavior

The existing SDL input frontend now submits console text to actual `Cbuf_AddText`
and executes it with `Cbuf_Execute` on host ticks. Registration, tokenization,
quoted delimiters and `wait` are production code. Supported local commands include
`echo`, `status`, `discover IPv4:port`, `net_restart`, one-ASCII-key `bind`, actual
`+attack`/`-attack` and `+forward`/`-forward` handlers, plus core command/dvar
operations. Enter submits the lightweight editor and clears the field; this is
still not the full game-console autocomplete/history UI. Unknown commands are
reported and never forwarded to a server. No shell process executor is added.

The headless and SDL frontends now instantiate the same exclusive `Runtime` owner.
It initializes real main-thread TLS/critical sections, Cmd/Cbuf, dvars and NET.
There is one monotonic process clock, one set of logging/allocation/error services,
and real registered dvars instead of duplicate false-value input globals. The
platform-only window recorder retains its small independent services; it does not
claim engine startup. The integrated input target no longer links those duplicate
services. Obsolete headless startup wrappers were removed.

`Cmd_InitCore` registers core facilities without importing asset/file-dependent
`exec`/`dumpraw`. Full game `Cmd_Init` retains its existing registration order and
features. Core dvar registration uses actual production functions; full game dvar
registration remains unchanged in its normal provider. Command services separate
script/server/forwarding dependencies from the registry/executor. The default
game adapter preserves existing behavior; the standalone provider has no hosted
server, reports unknown commands, and explicitly advertises unavailable script
monitoring. Standalone profiler hooks do not claim a profiler was initialized.
Dedicated/game builds retain their normal profiling configuration.

Focus loss and shutdown discard queued commands and `wait`, clear actual key,
button/pulse and mouse state, and prevent delayed actions from reactivating input.
Allocated bindings are freed with actual `Key_Shutdown`. Runtime teardown clears
command records/tokenizer/buffers, cancels any platform ticket, closes NET before
freeing dvars, and nulls owned dvar pointers. Repeated ownership is tested. Engine
errors unwind through the frontend: explicit normal/exception cleanup restores tokenizer/execution ownership, the
runtime drops pending commands and rethrows. No new nontrivial automatic objects
are crossed by the game's `longjmp` path; its existing `Cmd_ComErrorCleanup` remains
intact. Regression tests exercise both exception and actual setjmp/longjmp recovery.

A reusable `DiscoverySession` drives the existing production NET path from either
frontend. It sends only read-only `getinfo`, validates numeric endpoints, source
and challenge, and consumes at most eight full datagrams per nonblocking poll.
The SDL loop continues processing commands/input between polls. Network restart
cancels the old transaction before closing/reopening the socket. Discovery text
never enters Cbuf, and a response never advances client admission state. Only one
receiver owns this client socket at a time; future gameplay receive integration
needs central packet dispatch rather than competing readers.

## Reproduced and fixed command defects

- Empty command registration dereferenced null, and the final/sole command was
  skipped because iteration tested `itr->next` instead of `itr`.
- Oversized commands were clamped to 4095 bytes but consumed using the shortened
  length. Their remaining suffix could execute as a separate command. Client,
  server and synchronous buffer paths now discard the whole oversized physical
  command while retaining the following delimiter-separated command. The valid
  4095-byte boundary remains accepted.
- `wait` accepted negative/junk/overflow values through `atoi`, allowing indefinite
  stalls. It now validates a nonnegative integer in range, reports invalid input,
  and leaves queue progress intact. Zero, positive and default-one behavior remain.

Each was reproduced against the prior production behavior before repair. Evidence:
`command-baseline.json`, `command-wait-baseline.log`, `command-sync-baseline.log` in
`/workspace/kisakcod-client-command-host-results/`.

## Build and run

Use the same SDL/multilib setup and default-OFF window option from
[linux-client.md](linux-client.md). Existing targets evolve; no additional parallel
frontend is introduced.

```sh
cmake --build build-linux/window --target KisakCOD-client-input KisakCOD-client-command-test KisakCOD-client-runtime-test -j4
./build-linux/window/bin/KisakCOD-client-input --trace --run-ms 10000
./build-linux/window/bin/KisakCOD-client-input --trace --server 127.0.0.1:28960 --run-ms 3000
SDL_VIDEODRIVER=dummy ./build-linux/window/bin/KisakCOD-client-input-test
./build-linux/window/bin/KisakCOD-client-command-test
./build-linux/window/bin/KisakCOD-client-runtime-test
```

The discovery destination is always explicit. `--frame` still exits 5 and names
unavailable real game-frame services. Window close, Escape and SIGINT/SIGTERM use
normal cleanup. The X11 checker adds `--commands` alongside `--engine-input` and
`--relative`; it sends local commands using actual window keyboard events.

## Final validation

Evidence directory: `/workspace/kisakcod-client-command-host-results/`.

| Check | Debug and RelWithDebInfo result | Evidence |
| --- | --- | --- |
| Window/runtime aggregate | 82/82 each | `window-*-final.log` |
| Actual SDL/input/Cbuf lifecycle | 40/40 each | aggregate logs |
| Production command regression | 28/28 each | aggregate logs, `command-fixed-debug.log` |
| Shared runtime/real UDP lifecycle | 42/42 each | aggregate logs, independent runtime log |
| Prior SDL window/queue suite | 34/34 each | aggregate logs |
| Client-core aggregate including headless sockets | 471/471 each | `core-*-final.log` |
| Checker driver | 8/8 | `driver.log` |
| Real X11 commands/input/network/shutdown | passed each | `x11-*-final.log` |
| Dedicated default-OFF build + supported aggregate | builds passed, 12/12 each | `server-*.log` |
| Authorized real server discovery, both frontends | all four runs passed | `live-engine-discovery/report.json`, `live-engine-discovery.log` |

The input suite now uses the actual command executor to invoke IN handlers,
replacing the prior command-argument fixture. It proves delayed actions are removed
on focus loss and fresh commands execute afterward. Runtime tests exercise real
UDP cancellation, stale challenge rejection, eight-packet fairness, timeout,
restart, error unwinding and full ownership recreation. External X11 tests verify
queued `wait` ordering, local command execution, invalid/unknown commands, source
pinning, command-like network data remaining inert, pending-query cancellation,
and old-port rebinding while the client remains alive after `net_restart`.
Closing with pending commands does not execute them.

A fresh authorized asset server on `mp_crash` with default authentication answered
both frontends in both configurations. Headless exited 3 with unavailable auth;
the SDL dummy input frontend exited 0 with received discovery, zero held keys and
unavailable game frame. All reported `connected=false`; the owned server quit
with exit 0. The first probe was scheduled before the release headless binary
existed and stopped with a missing-file error, then cleaned its owned server.
That harness timing failure is retained in `live-engine-discovery-initial*`;
the final four-run report uses the finished binaries.

Default adapters/dvar sources compiled with MP, SP and dedicated configurations.
The existing config execution ownership regression passed. Independent review
covered the command fixes and cumulative ownership architecture. A prior receive
fixture needed an explicit command-services provider after the seam changed;
every method still aborts on unexpected use, preserving its original unsupported
command-execution boundary rather than weakening assertions.

Dedicated validation excludes `handshake_auth`, `server_test_clients`, and `udp`
under the existing direct-ELF32/sanitizer/shell limitations. These are 12 supported
baseline checks, not the separate server-reliability branch's full integrated
result. Server checkpoint `b5e3d408` and its patch remain independent. Default-OFF
builds retain no SDL dependency.

## Remaining limits and next step

This is real input/command/network startup, not playable native CoD4. No fake
connected/active state, game frame, renderer, audio, cgame startup or authentication
success is supplied. Linux authentication remains unavailable; no proprietary
assets, credentials, auth bypass or login behavior changed. The authorized
`libsteam_api.so` runtime/provider is still absent. Discovery is not gameplay.

Project binaries were run as ELF32 through the existing QEMU adapter; native
hardware/GPU and 32-bit sanitizer validation remain unverified. X11 input is
synthetic XTest input into actual windows. Dependency provenance remains the
previous official extracted Debian SDL/Xvfb/xdotool packages.

The next architectural step is a shared client packet dispatcher and real engine
startup state machine that can own discovery and connection traffic, followed by
bounded renderer/platform initialization. Enabling `CL_Frame` requires its actual
hunk, DevGui/voice, download and cgame services, not successful no-op substitutes.
