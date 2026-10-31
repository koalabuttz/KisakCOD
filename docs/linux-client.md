# Native Linux client

## Status

There is **no playable native Linux client**. The opt-in foundation now includes
eight full production client translation units, asset-free runtime integration tests,
a runnable Linux discovery diagnostic, and an independently opt-in SDL2 window/input shell. The diagnostic starts real UDP/event
handling and sends read-only `getinfo`; it explicitly reports `connected=false`
and unavailable Linux authentication. It does not initialize `CL_Frame`, load game
assets, authenticate, or join a server.

The default native Linux build is still the dedicated multiplayer server. This
window option adds no SDL dependency to it. The shared event queue is extracted without changing its normal overflow or console-polling behavior.

## What the milestone builds

`KISAK_BUILD_LINUX_CLIENT_CORE` (default `OFF`) adds eight targets:

| Target | Kind | Purpose |
| --- | --- | --- |
| `KisakCOD-client-core` | OBJECT library | Compiles eight production client TUs |
| `KisakCOD-client-netchan-test` | executable | Asset-free test of the real codec |
| `KisakCOD-client-state-test` | executable | Real command-parser and key-state runtime checks |
| `KisakCOD-client-packet-test` | executable | Production command emission, server decoding and prediction history integration |
| `KisakCOD-client-auth-test` | executable | Challenge formatting and real resend state machine |
| `KisakCOD-client-receive-test` | executable | Production reader/tokenizer/receive lifecycle and connection guards |
| `KisakCOD-client-platform-test` | executable | Full client-main compilation and portable setenv behavior |
| `KisakCOD-client-headless` | executable | Read-only UDP server discovery, no game admission |

The object library compiles these files **unmodified**, from their production
source paths:

- `src/client_mp/cl_net_chan_mp.cpp` — client netchan command encode/decode
- `src/client_mp/cl_parse_mp.cpp` — client message parsing
- `src/client_mp/cl_input.cpp` — client input/command generation
- `src/client_mp/cl_cgame_mp.cpp` — cgame state publication and history consumers
- `src/client_mp/cl_auth.cpp` — challenge formatting and platform authentication boundary
- `src/client_mp/cl_connection_mp.cpp` — production connection resend state machine
- `src/client_mp/cl_connection_receive_mp.cpp` — connectionless dispatch and timeout lifecycle
- `src/client_mp/cl_main_mp.cpp` — full client-main TU, with portable environment handling

Note the paths: the multiplayer client sources live under `client_mp/`.
`src/client/cl_input.cpp` is the singleplayer/Windows-client variant and is
*not* what this target builds. `scripts/mp/mp_files.cmake` already lists
`src/client_mp/cl_input.cpp` in `CLIENT_MP`, so the target matches the
production multiplayer build rather than inventing a parallel file set.

Compile settings are the dedicated server's Linux flags, **minus**
`KISAK_DEDICATED` and `DEDICATED`, which is the whole point: the same sources
compiled as the client side.

Do not add a file to this list without its own compile proof. "It looks
portable" is not proof.

### Why `KISAK_OPENAL` appears here

The client build has no `KISAK_DEDICATED` define, and `src/sound/snd_public.h`
selects its Windows-only `deps/msslib/mss.h` include on that basis:

```c
#if defined(__linux__) && defined(KISAK_DEDICATED)
#include <linux/compat.h>
#elif !defined(KISAK_OPENAL)
#include <msslib/mss.h>
#else
// portable typedef shim, no mss.h, no windows.h
```

Without a define, the client compile pulls `mss.h` and fails on a Windows-only
header. The target therefore sets `KISAK_OPENAL`, which selects only the
third branch — a block of portable Windows type shims.

Be precise about what this does and does not mean:

- It is **only** a header-include switch to get past `mss.h`.
- **No** sound source is compiled into this target.
- **No** audio backend is built, linked, or verified by it. Nothing here says
  anything about whether OpenAL or Miles works on Linux.

This is a build define, not a backend decision. Renderer and audio selection
remain out of scope.

## Running it

Keep this out of the shared build tree. The dedicated binary lives at
`bin/KisakCOD-dedi`, and `bin/` is the default runtime output directory — a
build there can replace a running server's binary. Configure a private build
directory with its own runtime output:

```sh
cmake -S . -B build-linux/client-core \
    -DKISAK_PLATFORM=linux \
    -DCMAKE_BUILD_TYPE=Debug \
    -DKISAK_BUILD_LINUX_CLIENT_CORE=ON \
    -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++ \
    -DCMAKE_RUNTIME_OUTPUT_DIRECTORY="$PWD/build-linux/client-core/bin"

cmake --build build-linux/client-core \
    --target KisakCOD-client-core KisakCOD-client-netchan-test KisakCOD-client-state-test KisakCOD-client-packet-test KisakCOD-client-auth-test KisakCOD-client-headless KisakCOD-client-receive-test KisakCOD-client-platform-test -j4

./build-linux/client-core/bin/KisakCOD-client-netchan-test
./build-linux/client-core/bin/KisakCOD-client-state-test
./build-linux/client-core/bin/KisakCOD-client-packet-test
./build-linux/client-core/bin/KisakCOD-client-auth-test
./build-linux/client-core/bin/KisakCOD-client-receive-test
./build-linux/client-core/bin/KisakCOD-client-platform-test
```

Requires the same Clang and multilib toolchain as the native Linux server
(`clang++ -m32` must link). Use a fresh build directory when changing compilers:
CMake may reset cached options during that change. The checker detects a lost
client-core opt-in rather than silently testing an unrelated target.

The checker wraps all of the above and asserts the dedicated binary was not
modified:

```sh
python3 scripts/tests/test_linux_client_core.py            # full: static + build + run
python3 scripts/tests/test_linux_client_core.py --static-only
```

It is deliberately **not** registered in `scripts/tests/run_linux_checks.py`.
The Linux workflow runs it separately for Debug and RelWithDebInfo, so the normal
server checks do not silently opt into client targets. Hosted CI results still
need to be checked separately from local validation.

The checker verifies generated compile commands for all eight client-core
translation units (`-m32`, `KISAK_MP`, no dedicated defines), builds in an isolated
output directory and executes all six runtime tests plus real socket tests. The state-test compile commands
are also checked for production source paths, client flags and `RELEASE_ASSERTS`. Its default-OFF check uses a
separate temporary tree. Compiler-name checks can run without a build:

```sh
python3 scripts/tests/test_linux_client_core.py --self-test
python3 scripts/tests/test_linux_client_core_driver.py
python3 scripts/tests/test_linux_client_core.py --build-type RelWithDebInfo \
    --build-dir build-linux/client-core-relwithdebinfo
```

## What the netchan test proves

`tests/linux_client_core/netchan_client_core_test.cpp` links the production
`cl_net_chan_mp.cpp` and `sv_net_chan_mp.cpp` and drives the four codec
functions: `CL_Netchan_Encode`, `CL_Netchan_Decode`, `SV_Netchan_Encode`,
`SV_Netchan_Decode`.

Cases covered:

- **Golden vectors** — independently calculated expected bytes for known
  challenge/serverId/sequence and command text, not outputs captured from the
  implementation under test.
- **Empty commands** — an empty command entry resets the ring index every
  iteration, so the payload is XORed with one constant byte. Both directions.
- **Roundtrip, both ways** — client encode → server decode, and server encode
  → client decode, across a realistic payload.
- **Command ring wrap** — the ring is 128 entries indexed with
  `& (MAX_RELIABLE_COMMANDS - 1)`; sequence 127 and 128 must select different
  slots, and a roundtrip must still hold across the wrap.
- **Key material sensitivity** — challenge, serverId and message sequence each
  change the key stream, and restoring them reproduces it exactly.
- **Zero length** and a **1024-byte payload** that wraps the command ring many
  times.

One detail worth recording, found while writing these cases: the client side
selects its key text with `serverCommandSequence` but mixes
`serverMessageSequence` into the key. They are different fields; a test that
sets only one measures the wrong thing.

### How the small doubles stay honest

The two production TUs normally reference the profiler, `Netchan_Transmit`,
`Com_Printf`, `svs`, and more. None of that is stubbed. The objects are
compiled with `-ffunction-sections -fdata-sections` and linked with
`-Wl,--gc-sections`, so every function the test never calls is discarded
rather than faked. Only these boundary symbols exist:

- `clients[]` — the real `clientActive_t` array that the inline
  `CL_GetLocalClientGlobals()` returns from
- `CL_GetLocalClientConnection()` — returns an owned `clientConnection_t`
- `MyAssertHandler()` — aborts loudly; the test builds with `RELEASE_ASSERTS`
  so the codec's own `iassert` invariants are actually evaluated

### What it does not cover

- **The transmit wrappers.** `CL_Netchan_Transmit` and `SV_Netchan_Transmit`
  call into `qcommon/net_chan_mp.cpp`, which drags in the whole netchan
  subsystem and its socket layer. Reaching them from an asset-free test would
  mean faking that layer, so they are not called. Instead the checker pins
  their command-header offsets statically from the production sources — the
  client encodes at `data + 9, length - 9` and the server at
  `data + 4, length - 4`. That fails if the header size ever changes, without
  inventing a fake call.
- **Transport and full game state.** Fragmentation, ordering, socket I/O and
  handshake are outside the codec test. The separate state test below covers
  bounded parser/input behavior, not the full client loop.

## Command parser and input runtime checks

`client_state_test.cpp` links the actual `cl_parse_mp.cpp`, `cl_input.cpp`,
`msg_mp.cpp` and `q_shared.cpp` translation units with section GC. Message
readers and string cleaning/copying are production code, not doubles. Independent
wire bytes exercise command admission, duplicate/stale sequences, ring wrap,
every truncation of a command, complete retransmission after rejection,
preexisting overflow, bounded/oversized strings, following-byte preservation,
all split-buffer boundaries, and untouched history/connection fields.

This exposed a real defect: `MSG_ReadString` returns a terminated string even
when input ends early. `CL_ParseCommandString` used to commit that partial text
and its newer sequence before the enclosing parser discarded the overflowed
message. It now checks overflow before changing command history. Valid commands
and the existing 1023-character output limit retain their previous behavior.

Input tests run production `IN_KeyDown`, `IN_KeyUp`, `CL_KeyState`, attack
command wrappers, `CL_UpdateCmdButton` and `CL_ClearKeys`. They cover repeat
presses, two keys bound to one button, third-source rejection, unrelated releases,
time accounting across frames, missing timestamps, clamped fractions, short
presses between frames, pulse consumption, held-button persistence and clearing.
No input behavior was changed by this milestone.

Boundary doubles supply connection lookup, command arguments, frame clocks,
diagnostic printing and fatal assertion handling. The private key-state array
is reached through real attack/clear wrappers. This does not test real keyboard
or mouse devices, window events, prediction, snapshot parsing, complete server
message parsing or command execution. Assertions remain active in both build
configurations. Native 32-bit sanitizer and graphical-client validation remain
required separately.

The checker requires both named parser/input result sections, zero failures,
nonempty sections and consistent totals. Its unit tests reject missing/empty
sections, contradictory totals and nonzero process exits. The default build
still exposes none of the optional client targets and the dedicated executable
fingerprint is checked even when a client build fails.

## What still blocks a real native client

Compiling eight client TUs is not a client. In rough order of weight:

1. **Engine startup integration.** A real SDL2 window/input host now exists, but
   its shell does not drive `CL_Frame`, client key/UI consumption, or game startup.
   Steam-error handling still has no supported Linux authentication provider.
2. **D3D9 renderer.** The rendering path makes real D3D9 calls. This is not a
   stub-shaped problem.
3. **Renderer backend choice** — out of scope for this milestone. Nothing here
   selects or recommends a backend.
4. **The rest of the client TUs**, each of which needs its own compile proof
   before it can join the object library.

The next connection milestone needs a supported Linux ticket provider and an
engine host that drives the real connection receive/state loop. The diagnostic
does not supply those services. Keep full-TU compile checks and production runtime
tests while integrating that host.

## Relationship to the dedicated build

- `KisakCOD-dedi` retains its normal source selection; shared event-queue code is
  now in `sys_events.cpp`, with console polling still in `main.cpp`.
- With `KISAK_BUILD_LINUX_CLIENT_CORE=OFF` (the default) the targets do not
  exist at all — verified by the checker against `cmake --build --target help`.
- The Windows build path is not touched.
- The checker fingerprints `bin/KisakCOD-dedi` before and after and fails if it
  changed, so a running dedicated server is never disturbed.

### Packet and prediction integration checkpoint

`client_packet_test.cpp` exercises production cgame setters, `CL_FinishMove`,
`CL_WritePacket`, Huffman compression, `CL_Netchan_Transmit` encoding,
`SV_Netchan_Decode`, message readers, and the prediction/command history consumers.
It crosses the 128-command, 32-packet and 256-prediction rings with 300 successive
packets, checks command backlog capping, non-delta fallback, repeated prediction
times, and expired/future history requests. See
[the integration checkpoint](progress-2026-10-31-client-integration.md) for limits.

The transport is a capture double at `Netchan_Transmit`; no socket, fragment
assembly, authentication, actual connection handshake, renderer, cgame simulation,
or two-player gameplay is exercised. Command scheduling/ring insertion is supplied
by the harness. Temporary allocation and host services are controlled doubles.

### Runnable discovery diagnostic

```sh
./build-linux/client-core/bin/KisakCOD-client-headless --server 127.0.0.1:28960 --timeout-ms 3000
```

There is no default destination. The endpoint must be an explicit IPv4 address and
port; the timeout is 1–10000 milliseconds. This sends one unauthenticated `getinfo`
query and accepts only a response from that endpoint with the matching challenge.
Malformed traffic does not reset the deadline. No ticket, challenge-auth request,
or connect packet is sent. Linux currently prints `auth=unavailable`; successful
discovery exits **3**, timeout **4**, transport failure **5**, invalid arguments
**2**, and help **0**. Every status explicitly states `connected=false`.

The diagnostic now uses production `NET_Init`, `NET_OutOfBandPrint`,
`NET_GetClientPacket`, `NET_Sleep`, `NET_Shutdown`, dvar registration and Linux
TLS/locking. It prints `network=engine-udp`. “Sent” means the engine API accepted
the query; only a valid reply proves discovery. It still does not host `CL_Frame`
or the connection dispatcher. Logging, allocation, clock and absent console/profile
services are explicit diagnostic adapters. The real receive/resend lifecycle runs
in a separate fixture with controlled game/transport services, not inside the
runnable discovery path. See the [lifecycle checkpoint](progress-2026-10-31-client-lifecycle.md)
for confirmed bugs, valid-wire coverage, render-thread work and remaining boundaries.


## Runnable SDL2 window/input shell

`KISAK_BUILD_LINUX_WINDOW_HOST=ON` independently adds `KisakCOD-window-platform`,
`KisakCOD-client-window`, and `KisakCOD-window-test`. It defaults OFF and needs
SDL2 >=2.0.18 development files and runtime matching the project's 32-bit build.
The client-core option does not imply this option or require SDL.

```sh
cmake -S . -B build-linux/window \
    -DKISAK_PLATFORM=linux -DCMAKE_BUILD_TYPE=Debug \
    -DKISAK_BUILD_LINUX_WINDOW_HOST=ON \
    -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++ \
    -DCMAKE_RUNTIME_OUTPUT_DIRECTORY="$PWD/build-linux/window/bin"
cmake --build build-linux/window --target KisakCOD-client-window KisakCOD-window-test -j4
./build-linux/window/bin/KisakCOD-client-window --trace --run-ms 10000
SDL_VIDEODRIVER=dummy ./build-linux/window/bin/KisakCOD-window-test
python3 scripts/tests/test_linux_window.py --build-dir build-linux/window
```

Set `SDL2_DIR` to the matching SDK's `lib/.../cmake/SDL2` directory if CMake cannot
find it. Keep `-m32` if overriding toolchain flags. Repeat with a separate
RelWithDebInfo build directory. The checker defaults to SDL's dummy driver;
`scripts/tests/test_linux_window_x11.py --help` describes the separate private
Xvfb/XTest integration test and its optional `--relative` capture checks.

The shell presents a plain SDL window surface, exits on close or Escape, and
supports `--frames`, `--run-ms`, `--title`, `--trace`, and `--relative`. Capture
failure is reported honestly; focus loss releases capture and focus gain restores
the requested mode. `window=ready` is emitted after the first successful surface
presentation, when the native window is ready for external automation. The shell
requests software framebuffer presentation; it initializes no game renderer.

The host translates SDL events through real `Sys_QueEvent` and shared engine
locking. Its consumer is an explicit reporting recorder, not `CL_KeyEvent`,
`CL_CharEvent`, or `CL_Frame`. The mouse callback has the actual `CL_MouseEvent`
signature; the mandatory reset callback has the `Key_ClearStates` signature.
A future game host must supply those consumers and drive the engine lifecycle.
The discovery diagnostic and window shell are still separate programs.

Focus loss, minimize and shutdown discard queued key/text events, preserve queued
console payload ownership/order, and reset consumer input state. This recovers
from a key-up lost to the existing 256-event ring overflow. Timestamps are
calibrated from SDL's clock into engine milliseconds, including wraparound.
Physical modifier keys sharing an engine binding retain independent held state.
Text input validates UTF-8 and emits printable Latin-1 for the existing byte-based
engine field API; other Unicode scalars are counted and rejected, not truncated.
One host owns SDL's process-wide event queue; callbacks must not throw and their
sink must outlive the host.

See [the window checkpoint](progress-2026-10-31-client-window.md) for the measured
SDL, X11, dedicated-build and integration limits. These tests do not establish
hardware input, GPU rendering, authenticated joining, or native gameplay.


### Production input-consumer host

The same window opt-in now also builds `KisakCOD-client-input`,
`KisakCOD-client-input-test`, and `KisakCOD-shutdown-signal-test`.
Run `KisakCOD-client-input --trace --relative --run-ms 10000` for actual engine
key/character/mouse handling and disconnected field editing reported in stdout.
It uses real `playerKeys` and field state; the earlier `KisakCOD-client-window`
remains the platform-only recorder. The new host still presents a plain surface,
not game UI. Its terminal-cell layout and unavailable console execution/clipboard/
autocomplete services are explicit. The normal game service adapter preserves the
existing subsystem calls.

Focus/quit clear actual key, button and mouse state; each input tick consumes
mouse deltas. SIGINT/SIGTERM enter normal shutdown and restore previous handlers.
`--frame` returns exit 5 with the unavailable `CL_Frame` dependencies, without
pretending a game loop ran. No connection state is advanced. The window checker
includes both hosts and the real-consumer lifecycle tests; the external X11
checker selects the new path with `--engine-input`.

See [the input-host checkpoint](progress-2026-10-31-client-input-host.md) for
implementation boundaries, build commands, evidence and remaining startup work.
