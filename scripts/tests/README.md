Native Linux dedicated-server checks are documented in
[the Linux build guide](../../docs/linux-dedicated.md#asset-free-checks). They
include an ELF32/bootstrap smoke test, native runtime and UDP tests, fastfile
short-read regressions, non-interactive debugger checks, random-helper/item-drop
range checks (`test_random_range.py`), and gameplay-lifecycle checks
(`test_gameplay_lifecycle.py`). The random-range check extracts production
helpers, checks Windows 15-bit parity and wide Linux `rand()` inputs, and bounds
dropped item and grenade launch velocities; it does not simulate complete deaths
or physics. The gameplay-lifecycle check extracts the shipped `DeathGrenadeDrop`
and `LaunchItem`/`G_RunItem` bodies and covers held versus offhand grenade
selection, the remaining fuse, model, owner and 40-unit launch origin, the
grenade-death perk (known, unknown and suicide-suppressed), and item
initialization, the 50 ms trajectory step, settling, ground landing and the
freed-entity guard. It also extracts the shipped `G_SetOrigin` and
`BG_EvaluateTrajectory`, so placement really resets the trajectory and the item's
step is really integrated over the frame rather than handed a canned sample. It
drives those bodies with checked doubles for spawning, tracing and weapon defs,
so it measures the engine's own logic without assets or a running server;
collision geometry and capsule sweeps are still doubles, so no real physics
collision is claimed. These native checks require the
32-bit Linux toolchain and are separate from Windows runtime checks. After
building, run `python3 scripts/tests/run_linux_checks.py --build-dir build-linux
--binary bin/KisakCOD-dedi` for the full asset-free set. The opt-in
`test_linux_server_lifecycle.py` additionally exercises restart/rotation, live
loopback queries and resource samples with installed licensed assets; it is not
run on public CI. Its failure-cleanup/query-parser unit tests are asset-free.
`test_server_handshake_auth.py` exercises production authentication parsing,
handshake functions and connect-version admission (including the opt-in
`sv_allowProtocol6` and `sv_allowUnverifiedGuids` switches) without assets; the
UDP suite also checks the startup-only loopback bypass against real sockets. The
opt-in `test_linux_handshake.py` uses installed assets to verify real UDP
admission, default rejection, unsafe bind failures, optional protocol-6 admission
for a local-test client, and the retail-GUID opt-in for protocol 1 and protocol 6
clients. It decodes no gamestate and does not establish general stock/CoD4x
client compatibility.

`test_server_remote_input.py` compiles the production
`src/server_mp/sv_remote_input.h` rules 64- and 32-bit: `rcon` is refused until
`rcon_password` is set and matches exactly (an unset password used to accept
`rcon "" <command>` from any address), and a `download <name>` client command
is served only for a plain relative `.iwd`/`.ff` path with no `..`, `:` or
leading separator (any file under `fs_homepath` used to be downloadable).

`test_server_test_clients.py` checks the extracted internal bot functions with
controlled connection and movement services; the native runner runs its 32-bit
sanitizer configuration. The opt-in `test_linux_bots.py` uses stock bot/team/class setup, requires repeated healthy
hold samples, and checks clean shutdown. The weapon-definition ownership and
bot-driver regressions are part of the asset-free runner. See
[Server-side test clients](../../docs/server-test-clients.md) for the exact
coverage and blocker.

The opt-in [native Linux client core](../../docs/linux-client.md) compiles actual
client translation units and runs asset-free client/server codec and bounded
command-parser/key-state tests. Message readers are production code. It is
checked separately from the dedicated-server suite and is not a playable client.

These shared asset-free regression checks require Python 3 and a C++20 compiler:

```sh
python3 scripts/tests/test_renderer_command_ownership.py
python3 scripts/tests/test_script_vm_field_stack.py
python3 scripts/tests/test_script_lexer_eof.py
python3 scripts/tests/test_shader_constant_order.py
python3 scripts/tests/test_shellshock_channel_metadata.py
python3 scripts/tests/test_script_loading_watchdog.py
python3 scripts/tests/test_voice_lifecycle.py
python3 scripts/tests/test_script_console_watchdog.py
python3 scripts/tests/test_exec_command_ownership.py
```

All scripts accept `--cxx` (or `CXX`) to select a compiler. They compile the
functions extracted from the current production source, with checked test
doubles for unrelated engine services. Assertions remain enabled.

The renderer check covers command-buffer ownership before initialization,
partial allocation, unvisited and aliased frame bindings, normal/swapped frame
cycles, and command-buffer cleanup/reallocation. Lighting, debug geometry and
scene cleanup are stubbed; complete renderer shutdown still needs a game check.

The lexer EOF check runs the production scanner/parser with zero and one local
clients in SP and MP. Empty/comment-only files, final tokens without a newline,
repeated parses, and malformed input must terminate without restarting an
exhausted source. It does not require a running client or game assets.

The VM check forces the global layout that exposed the false overflow, then
checks scalar and undefined fields, array getters that need two working values,
repeated getters, output-parameter cleanup and genuine stack overflow. It tests
the field bridge and stack bookkeeping, rather than executing complete scripts.

The shader check verifies destination ordering and destination/value pairing
through all 16 constant slots, with unused storage poisoned to expose accidental
reads. It exercises the ordering used by material comparisons; loading and
rendering real materials still need a game check. The extracted collection and
comparison helpers also check literal and named constants, equivalent/self
comparisons, reversed ordering signs and repeated comparisons.

The separate [D3DX runtime check](d3dx_shader_runtime.md) compiles and reflects
real shaders on Windows or Wine, including macros, includes, input/output
semantics, COM ownership and invalid-input diagnostics. It needs the x86 D3DX9_43
runtime but no GPU or game assets. Windows CI runs it in both configurations.

The shellshock check loads sound-channel metadata without an audio device,
verifies repeated/client reuse and fallback settings, and preserves warnings for
unknown channel names and errors for missing channel definitions.

The watchdog check excludes synchronous item-registration time during loading
without resetting script time already spent. It checks SP and MP precache calls,
the five-second boundary, real pure/interleaved loops, clock wrap and unchanged
runtime/precache restrictions.

The console watchdog check applies the same loading-time accounting to script
print output. It covers SP/MP print and println, profiling output, suppressed
messages, nested accounting, clock wrap and genuine repeated-print loops. It
preserves the script budget already consumed and normal runtime timing.

The voice check exercises disabled/dedicated startup, device and codec failure
rollback, repeated shutdown, off/on transitions and capture failures. It also
checks COM buffer ownership and bounded sample storage through 1,000 capture
stop/start cycles. Audio devices and codec allocation use checked test doubles;
physical microphone/playback behavior still needs a device test.

The exec check runs production disk/fastfile dispatch and command parsing across
map-memory resets and asset unloading. It covers trailing commands, nested
configs, missing/empty input, allocation failure, startup order and repeated
restarts while preserving synchronous execution and existing wait semantics.
Real nonlocal aborts also check reverse-order ownership cleanup, repeated and
reentrant recovery, and bounded command nesting.

GitHub Actions runs these checks alongside the Windows builds, the native
bootstrap check, the asset-free world surface checks and the native D3D9 boundary
checks (below). Loading a map and
quitting with licensed assets remain separate application checks.

## World surface checks

```sh
python3 scripts/tests/run_world_surface_checks.py --config Debug
python3 scripts/tests/run_world_surface_checks.py --config RelWithDebInfo
python3 scripts/tests/run_world_surface_checks.py --list
```

This runner performs no CMake configure and no engine build. It compiles nine
standalone test translation units directly with `-m32 -std=c++20 -Werror` and runs
them, then runs the bundle prepare script, for ten checks:

| Check | Source | Covers |
| --- | --- | --- |
| `world_surface_inventory` | `tests/linux_asset_export/world_surface_inventory_test.cpp` | inventory allowlist, declaration routing, slice bounds, bundle helper arithmetic |
| `world_surface_context` | `tests/linux_renderer/world_surface_context_test.cpp` | shader-constant context for one unlit surface draw |
| `world_surface_bundle` | `tests/linux_renderer/world_surface_bundle_test.cpp` | bundle loader, shader framing walk, vertex accessors |
| `world_surface_oracle` | `tests/linux_renderer/world_surface_oracle_test.cpp` | oracle hashing, weights, sampling, colour and pixel arithmetic, sample selection |
| `world_lit_bundle` | `tests/linux_renderer/world_lit_bundle_test.cpp` | lit loader refusals: program pair hash and length, widened geometry counts, index walk, colour-map sampler, exact constant set, lightmap layout |
| `world_frontend_invariants` | `tests/linux_asset_export/world_frontend_invariants_test.cpp` | the rules the offline world front-end probe holds each frame to, and its spawn-point camera scan ([guide](../../docs/linux-world-frontend.md#a-real-map-the-offline-front-end-probe)) |
| `world_device_rules` | `tests/linux_asset_export/world_device_rules_test.cpp` | the rules the world device probe holds each texture, shader and vertex/index buffer to ([guide](../../docs/linux-world-device-probe.md)) |
| `world_draw_rules` | `tests/linux_asset_export/world_draw_rules_test.cpp` | what the world draw probe holds a drawn frame to: the back-buffer measurement, the colour, coverage, brightness, repeat and camera rules, and the draw counts, on frames built in memory ([guide](../../docs/linux-world-draw-probe.md)) |
| `world_image_capture` | `tests/linux_asset_export/world_image_capture_test.cpp` | capture bounds, name filter, duplicate rule, disabled path |
| `world_surface_prepare` | `test_world_surface_prepare.py` | the prepare step's trust boundary over archives it writes itself |

`world_image_capture` is the only case with a second translation unit: the capture
state lives behind a mutex in `src/linux/tools/world_image_capture.cpp`, so that
unit is compiled beside the test and linked with `-pthread`. It stays engine-free:
no engine header, no asset tree, no zone. Every other case is a single translation
unit that includes one header and opens no file except bundles it writes itself.

Configurations are explicit rather than inherited: `Debug` compiles at `-O0` and
`RelWithDebInfo` at `-O2`, both with `-g`. Each command runs in its own process
group with its own timeout, so a stuck check is killed instead of hanging a job. A
check passes only when the process exits 0, the produced binary really is ELF32/i386,
and its output contains no `FAIL` line.

A passing run says the helpers behave as their tests state. It does not claim that
any surface was rendered, that a captured frame was reproduced, or that the hosted
workflow itself ran. It needs no DXVK, SDL, GPU, Steam client, credentials, game
assets or network, which is why it can run on an ordinary hosted runner.

The four renderer tests are compiled here directly because
`tests/linux_renderer/CMakeLists.txt` requires the `dxvk-d3d9` and `sdl3`
pkg-config modules before it configures anything. That project also builds the two
GPU replay executables, which are a separate manual experiment; see
[tests/linux_renderer/README.md](../../tests/linux_renderer/README.md).

## Native D3D9 boundary checks

```sh
python3 scripts/tests/test_native_d3d9_boundary_runner.py
python3 scripts/tests/run_native_d3d9_boundary_checks.py --config Debug --dxvk-source <dxvk-2.7.1-checkout>
python3 scripts/tests/run_native_d3d9_boundary_checks.py --config RelWithDebInfo --dxvk-source <dxvk-2.7.1-checkout>
```

These check the opt-in boundary through which renderer code reaches DXVK Native
D3D9 (`src/linux/d3d9_native.h`). They need DXVK Native 2.7.1 **headers** only:
no DXVK build or library, no Vulkan, SDL, GPU, Steam client, credentials or game
assets, and no CMake configure. The boundary header is built with g++ and clang++
in both modes and disassembled to confirm the engine's stdcall survives DXVK's
headers; four sources must fail to compile for their stated reason; the
D3D9-bearing engine layouts must be identical in the stand-in and DXVK views; and
the production renderer units in
`scripts/platform/linux/native_d3d9_renderer_sources.txt` must compile through the
boundary and fail without it. See [docs/linux-native-d3d9.md](../../docs/linux-native-d3d9.md).
A pass does not claim a device, a draw, linking or the hosted run.

## D3D9 device probe and Win32 window layer

These are CMake targets, not a runner: configure with
`-DKISAK_BUILD_LINUX_NATIVE_D3D9=ON -DKISAK_BUILD_DEVICE_PROBE=ON`, the DXVK Native
headers, a D3D9 library (`KISAK_DXVK_NATIVE_LIBRARY`) and `SDL3_DIR`, then build
`KisakCOD-win32-window-test` and `KisakCOD-device-probe`
([guide](../../docs/linux-device-probe.md)).

```sh
SDL_VIDEODRIVER=dummy LD_LIBRARY_PATH=<sdl3-sdk>/lib <bin>/KisakCOD-win32-window-test
LD_LIBRARY_PATH=<dxvk-prefix>/lib32:<sdl3-sdk>/lib <bin>/KisakCOD-device-probe --frames 60
```

The window test checks the Win32 window and monitor calls `r_init.cpp` makes, as
implemented over SDL3, on SDL's dummy driver: no display, GPU, Vulkan or D3D9
library. The probe creates the device through `r_init.cpp` and needs DXVK Native
2.7.1, a Vulkan driver and a display. CI builds both against a stand-in D3D9
library whose `Direct3DCreate9` returns null, runs the window test, and checks the
probe fails closed at `stage=direct3d`; it never creates a device.

### World device probe

`KisakCOD-world-device-probe` (`-DKISAK_BUILD_WORLD_DEVICE_PROBE=ON`, on top of the
device and world front-end probes) loads the graphics zones and one map onto a
D3D9 device and checks every device object they asked for
([guide](../../docs/linux-world-device-probe.md)). Its rules have an asset-free
test in the world surface runner (`world_device_rules`). Without a device:

```sh
LD_LIBRARY_PATH=<sdl3-sdk>/lib:<stand-in dir> SDL_VIDEODRIVER=dummy \
  python3 scripts/tests/test_world_device_probe.py --probe <bin>/KisakCOD-world-device-probe
```

The script starts the engine from a temporary tree it creates and removes, holding
only a one-line stub `fileSysCheck.cfg` (the file the engine refuses to start
without), and runs the probe three times: an allowlisted map must stop at
`stage=direct3d` before any zone is queued, and no map and an unknown map must be
refused first. All three exit 1 and write nothing. It takes no game directory and
is meant for the stand-in library: with a working DXVK and a Vulkan device the
first run gets past `Direct3DCreate9` and the script fails.

### World draw probe

`KisakCOD-world-draw-probe` (`-DKISAK_BUILD_WORLD_DRAW_PROBE=ON`, on top of the
world device probe) draws views of one map through the engine's own renderer and
checks each frame it reads back
([guide](../../docs/linux-world-draw-probe.md)). Its frame rules have an
asset-free test in the world surface runner (`world_draw_rules`). Without a
device:

```sh
LD_LIBRARY_PATH=<sdl3-sdk>/lib:<stand-in dir> SDL_VIDEODRIVER=dummy \
  python3 scripts/tests/test_world_draw_probe.py --probe <bin>/KisakCOD-world-draw-probe
```

The script uses the same kind of temporary tree as the world device probe's and
runs the probe eight times. In the draw mode an allowlisted map must stop at
`stage=direct3d` before `R_Init` runs, and no map, an unknown map and a show time
over 60 seconds must be refused first. In the free-camera replay
(`KISAK_FREE_CAMERA_MAP`, [guide](../../docs/linux-free-camera.md#the-replay)) an
allowlisted map must stop at the replay's own `stage=direct3d`, and both map
variables at once, an unknown map and a show time must be refused first. All
eight exit 1 and write nothing.

## World front-end checks

```sh
python3 scripts/tests/run_world_frontend_checks.py --config Debug --dxvk-source <dxvk-2.7.1-checkout>
python3 scripts/tests/run_world_frontend_checks.py --config RelWithDebInfo --dxvk-source <dxvk-2.7.1-checkout>
python3 scripts/tests/run_world_frontend_checks.py --list
python3 scripts/tests/test_world_frontend_check_runner.py
```

`tests/linux_world_frontend/world_frontend_test.cpp` runs the production renderer
front end (scene and view setup, DPVS cell and portal visibility, the per-cell
AABB-tree and cull-group walk, BSP draw-surface batching and the merge) against a
ten-surface, two-cell synthetic world and checks 28 results, the batches derived
from the geometry. The runner compiles the test, its link boundary and 46
unmodified production units with one `-m32` line per configuration (`Debug` is
`-O0 -D_DEBUG`, `RelWithDebInfo` is `-O2 -DNDEBUG`, both with `RELEASE_ASSERTS`),
links them with `--gc-sections` and runs the ELF32 result. Production assertions
abort the run. `r_scene.cpp` and `r_buffers.cpp` are compiled through the native
D3D9 boundary, so the pinned DXVK Native 2.7.1 **headers** are needed
(`--dxvk-source` or `--dxvk-include-dir`, as for the boundary checks); no DXVK
library, D3D9 call, SDL, GPU, assets or CMake configure is involved. See
[the world front-end guide](../../docs/linux-world-frontend.md) for the link
boundary rules and what a pass does not claim.

## Client compile census and CRT shims

```sh
python3 scripts/tests/run_client_compile_census.py --cxx clang++ --jobs 4
python3 scripts/tests/test_client_compile_census.py
python3 scripts/tests/test_linux_crt_shims.py --cxx clang++
```

The census runs one syntax-only `-m32` pass with the client definitions over
every multiplayer client source and compares the result with
`client_compile_census_blocked.txt` in both directions: a listed source that now
compiles is a stale entry and an unlisted source that stops compiling is a
regression. Each blocked entry names what the compiler stops on. The
build-generated `buildnumber.h` is written by the build's own
`scripts/extern/write_build_number.cmake` into a private directory, so a fresh
checkout needs no prior build. It compiles, it does not link, so a pass is not a
claim that a client binary builds. The CRT
shim test covers the MSVC CRT names `src/linux/compat.h` supplies to client
sources (`_itoa`, the array `strcpy_s`, `_time64`, `_localtime64`), including the
fail-closed cases; `run_linux_checks.py` runs it as `crt_shims`.

## Free camera checks

```sh
python3 scripts/tests/test_free_camera_check_runner.py
python3 scripts/tests/run_free_camera_checks.py --config Debug
python3 scripts/tests/run_free_camera_checks.py --config RelWithDebInfo
```

Like the world surface runner this needs no CMake configure, engine build, DXVK,
SDL, GPU, Steam client or game assets. It compiles with `clang++ -m32` (the Linux
engine build's compiler, `--cxx` overrides it) and the production Linux client
flags, in three checks:

| Check | Source | Covers |
| --- | --- | --- |
| `production_math_object` | `src/universal/com_math_anglevectors.cpp` | the original AnglesToAxis/AngleVectors unit, unmodified, production flags only |
| `free_camera` | `tests/linux_free_camera/free_camera_test.cpp` | pose, mouse/key turning, movement, CG_UpdateFov projection, refdef fill, key bindings |
| `frame_owner` | `tests/linux_free_camera/frame_owner_test.cpp` | frame order against a recording renderer: resize/minimize, device loss/reset, load/reload/unload, shutdown, fail-closed paths, focus/capture, reference traversals, seeded randomized lifecycle |

The new sources and tests are built with `-Wall -Wextra -Werror`; the original
engine unit is not, because its existing warnings are the original source's. It is
linked with `--gc-sections`, as the production targets are, so its unused
AnglesToQuat does not pull in more of the engine. A pass says the camera and frame
ordering behave as tested against a fake renderer; it does not claim a map was
drawn or a real device was reset. See [docs/linux-free-camera.md](../../docs/linux-free-camera.md).

The window input that feeds the owner (`src/linux/client/view/free_camera_window.cpp`)
needs SDL, so it is not in this runner. It is built and run by
`test_linux_window.py` as `KisakCOD-free-camera-window-test`, with SDL2 and SDL3 and
the dummy video driver: a window made the way the engine's window layer makes one
is adopted by the window host and driven with synthetic SDL events, against the
same recording renderer.

For a sanitized run use GCC: `--cxx g++ --cxx-flag=-fsanitize=address,undefined
--cxx-flag=-fno-sanitize-recover=all`. Clang's sanitizer instrumentation keeps the
production unit's unused AnglesToQuat alive through `--gc-sections`, so a sanitized
Clang link stops at the missing AxisToQuat; that is a property of the sanitized
link, not of the checks.
