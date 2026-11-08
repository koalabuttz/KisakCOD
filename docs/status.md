# Linux port status

Last updated 2026-11-07 (master `7a3d61e` plus the armv7 dedicated server). This page is the current state of
the native Linux port and the other platform builds, the open pull requests and
the roadmap. The [docs index](README.md) lists the detailed guides;
older handoffs are in [history](history/README.md) and are not current.

Results marked *laptop* come from one developer machine (Intel Raptor Lake-U,
Mesa ANV, DXVK Native 2.7.1, SDL 3.2.10, OpenAL Soft) with the game files
installed. They are not CI evidence. Hosted CI builds and runs everything below
without a GPU or game data, against stand-in D3D9 and OpenAL libraries.

## What works

| Piece | Build | State |
| --- | --- | --- |
| Dedicated server, i386 | `build-linux.sh` (default) | Loads all 21 stock maps, hosts bots through map cycles, clean shutdown (laptop). Retail CoD4 1.7 and CoD4x 21.1 clients have each played a brief local session through the authentication opt-ins. [Guide](linux-dedicated.md) |
| Dedicated server, x86_64 | `-DKISAK_LINUX_ARCH=x86_64` | Same as i386: 21 of 21 stock maps, bots through map cycles, clean quit (laptop). [Guide](linux-x64.md#x86_64-dedicated-server) |
| Multiplayer client `KisakCOD-mp`, i386 | `-DKISAK_BUILD_LINUX_MP_CLIENT=ON` | Playable on a listen server (laptop): `+devmap mp_crash`, team and class menus, spawn, move, shoot, grenades, OpenAL sound, flashbang white-out, clean quit. Client prediction misses as often as the Windows build (see [Prediction](#prediction)). [Guide](linux-mp-client.md) |
| Multiplayer client, x86_64 | the same, with `-DKISAK_LINUX_ARCH=x86_64` | Playable on the same terms as i386 (laptop), including sprint and mantle animations and a clean quit. [Guide](linux-x64.md#x86_64-multiplayer-client) |
| Dedicated server, aarch64 (64-bit Arm) | `-DKISAK_LINUX_ARCH=aarch64` | Builds natively on Arm or cross-compiled from x86, boots without game data (qemu and hosted Arm CI). Not yet run with game data. [Guide](linux-arm64.md) |
| Multiplayer client, aarch64 | the client options with `-DKISAK_LINUX_ARCH=aarch64` | Builds and passes the asset-free client checks (qemu and hosted Arm CI) against a D3D9 stand-in; no aarch64 DXVK build yet, so it has not drawn a frame. [Guide](linux-arm64.md#building-the-aarch64-client) |
| Dedicated server, armv7 (32-bit Arm) | `-DKISAK_LINUX_ARCH=armv7` | Cross-compiled from x86, boots without game data under qemu-arm (hosted CI). First step toward a PS Vita port. Not yet run with game data. [Guide](linux-armv7.md) |
| Dedicated server, PS Vita | `-DKISAK_PLATFORM=vita` | Clang + VitaSDK build packed into `KisakCOD-dedi.vpk` (hosted CI job, artifact). Not yet run on a Vita. [Guide](vita.md) |
| Dedicated server, macOS (arm64 and x86_64) | `-DKISAK_PLATFORM=macos` | Builds with Apple Clang on hosted macOS runners (Apple silicon and Intel) and boots without game data to the first missing fastfile. Not yet run with game data or on a real Mac. [Guide](macos.md) |
| Movies | `-DKISAK_LINUX_CINEMATICS=ON` | Bink movies play through an FFmpeg-backed shim on i386 and x86_64 (laptop). Off by default. [Guide](linux-cinematics.md) |
| Steam authentication | `-DKISAK_LINUX_STEAM=ON` | Added by PR 85. Logs in through the running Steam client's `steamclient.so`; a probe got a ticket validated on i386 and x86_64, and an i386 client joined, reconnected to and was refused by (with Steam off) an i386 dedicated server; the same passed with x86_64 client and server, and across i386 and x86_64 in both directions (laptop, one Steam account). Off by default. [Guide](linux-steam.md) |
| Singleplayer `KisakCOD-sp`, i386 | `-DKISAK_BUILD_LINUX_SP_GAME=ON` | killhouse and cargoship load, leave the pregame by themselves and play about a minute each, clean quit (laptop). All 21 levels load, run 45 s and quit cleanly; a 4-level mission chain and devsave/loadgame pass (laptop sweep run 1, [levels](linux-sp-levels.md)). [Guide](linux-sp-game.md) |
| Windows x64 | MSVC `KISAK_WIN64` | sp, mp and dedi build and link (CI job "MSVC x64 build"); opt-in FFmpeg movies. On the Windows PC, mp_crash loads to class select and the dedicated server hosts bots. Singleplayer still crashes during level load (fixes in PR 95). |
| Windows ARM64 | MSVC `KISAK_WIN_ARM64` | sp, mp and dedi build and start up to the missing-game-data error on hosted Windows on Arm. Not run with game data. [arm64 track](linux-arm64.md) |
| Profiling | `KISAK_LINUX_TRACY` / `KISAK_WIN_TRACY` | Opt-in Tracy builds of the game clients on Linux and Windows. |

The client builds need DXVK Native, SDL3 3.2.10 or newer and OpenAL for the
target architecture; the guides above give the CMake lines. The game files
always come from your own install (`+set fs_basepath <game folder>`).

## Prediction

An early laptop session (2026-11-03) logged about five times as many
`cg_showmiss 1` prediction misses as a later Windows session (2026-11-04).
Follow-up laptop runs put that down to what happened in play, not to the Linux
build:

| Session | Play | Misses | Mean size |
| --- | --- | --- | --- |
| Windows PC, CI build | ~5 min by hand | 32 (34 error lines) | 0.47 units |
| Laptop, i386, by hand | ~2.5 min, incl. concussion grenades | 19 | 0.65 units |
| Laptop, i386, scripted moves | run, strafe, turn, jump, sprint, two passes | 0 | - |
| Laptop, x86_64, scripted moves | the same two passes | 31 | 0.14 units (max 0.34) |

Misses per minute by hand are about the same (6.4 Windows, 7.6 Linux). The
scripted rows are laptop run 13 (2026-11-06, master `342e6b5`), with the moves
typed into the console: CoD4 runs an `exec`'d config all at once, so `wait`
lines in it have no effect. The x86_64 player spawned somewhere else and spent
most of the script pushed against obstacles (it moved far less than the i386 player);
29 of its 31 misses came in one sprint phase. So the x86_64 row is not a clean
comparison with i386, but every miss was smaller than the Windows average.

Two build settings keep the movement math the same as the Windows build, which
matters because the client replays the server's movement code: x86 Linux builds
use SSE float math (`-msse2 -mfpmath=sse`, no x87 excess precision), and aarch64
builds use `-ffp-contract=off` (no fused multiply-add).

## What does not work yet

- **Joining a remote server in the default build.** It has no Steam ticket
  provider, so the client refuses to send an unauthenticated challenge, and the
  dedicated server rejects ordinary client tickets unless an explicit opt-in is
  set ([details](linux-dedicated.md#local-connection-testing-without-steam-authentication)).
  Build with `-DKISAK_LINUX_STEAM=ON` to authenticate through the Steam client
  running on the machine ([guide](linux-steam.md)). Not yet tried that way: a
  Windows client or server on the other end, two Steam accounts.
- **Singleplayer played by hand past two levels.** All 21 levels load and run
  unattended, and mission changes and save loading work from the console
  ([linux-sp-levels.md](linux-sp-levels.md)). Only killhouse and cargoship
  have been played by hand; night vision, ac130, the Javelin, slow motion,
  death and retry, and the victory screen are untried.
- **x86_64 singleplayer.** The 64-bit configuration refuses the SP game for now.
- **Windows x64 singleplayer.** Each PC run gets one step further into level
  load; the fixes so far are in PR 95.
- **macOS client.** The multiplayer client builds for Macs, with the Bink movie
  shim, and passes its asset-free checks in hosted CI against the D3D9 and
  OpenAL stand-ins; drawing
  waits on the renderer. The Vulkan core and its window probe run on Metal
  through MoltenVK on both hosted Mac runners
  ([macOS track](macos.md#vulkan-window-probe)).
- **arm64 client and Windows ARM64.** The aarch64 multiplayer client builds and
  passes its asset-free checks (qemu and hosted Arm CI) but has not drawn a
  frame: no aarch64 DXVK build yet. Windows ARM64 builds sp, mp and dedi with
  MSVC and starts up to the missing-game-data error on hosted Windows on Arm;
  not run with game data ([arm64 track](linux-arm64.md)).
- **HTTP downloads, voice chat and the remote script debugger** are missing from
  the Linux client; each answers as Windows does when that service is absent
  ([list](linux-mp-client.md#what-it-does-not-have-yet)).

## Security

The 2026-11-01 security audit is closed: PR 18 fixed findings 1 to 3 and PR 106
(2026-11-06) fixed findings 4 to 11, widened the download file name check and
built the Linux games and server with `_FORTIFY_SOURCE=2` and the usual
hardening flags (laptop soak run passed). PR 106 also made CI retry slow apt
mirrors instead of timing out.

## Other Linux pieces

These came before the playable client and still build and run in CI:

- The [free camera](linux-free-camera.md) and the
  [world draw probe](linux-world-draw-probe.md) fly and draw a map through the
  engine renderer without a player.
- The [client core](linux-client.md) test targets compile client translation
  units into small asset-free tests.
- The [standalone renderer](../tests/linux_renderer/README.md) replays single
  exported world surfaces and checks pixels against CPU predictions.
- The [Vulkan core and window probe](vulkan-core.md) (`-DKISAK_BUILD_VULKAN_PROBE=ON`)
  open a window and clear, present and resize it through Vulkan, or draw a
  small textured scene through the core's memory, upload, pass and pipeline
  paths and check its pixels. It is the base of the Vulkan back end; no game
  rendering goes through it yet. Hosted CI runs it on lavapipe (Linux) and
  MoltenVK (macOS); the clear probe also ran on the laptop and the PC GPU.

## Open pull requests

As of 2026-11-08.

| PR | What | Waits on |
| --- | --- | --- |
| 120 | Record the Vulkan probe's first GPU runs (docs) | Review. |
| 123 | Keep the Mac client's files in Application Support | Green CI. |
| 124 | Vulkan core memory, uploads, passes, pipeline cache and the probe's `--draw` test (first piece of the Vulkan Legacy back end) | Green CI and the laptop draw run (`renderer/vulkan-probe-laptop-run-2-steps.md`). |

## Laptop and PC runs still to do

- Sound listening run for the OpenAL fix (PR 102, merged): plan in
  `sound/laptop-sound-run-1.md`, with `+set snd_alDebug 1`.
- Singleplayer hand play on the levels the sweep passed (step 6 in
  [linux-sp-levels.md](linux-sp-levels.md)); sweep run 1 is done.
- Vulkan probe `--draw` on the laptop, x86_64 and i386 (PR 124).
- Vulkan probe with the Khronos validation layer (synchronisation checks) on a
  real GPU; the laptop and PC runs so far had no layer installed.

## Roadmap

In rough order:

1. **Land the open work above**.
2. **Linux singleplayer beyond two levels**: hand play of the swept levels,
   then x86_64 singleplayer.
3. **Renderer**: the shader converter (dxbc-spirv) and the D3D9 interface
   (M1a) are done. Next is the Vulkan back end with Legacy visuals behind that
   interface: the core's drawing pieces (PR 124), the shader translator, the
   Legacy adapter, then mp_crash drawn through Vulkan and compared with DXVK on
   the laptop and the PC. The Modern path comes after (plan in the project
   files, `renderer/modern-renderer-plan.md`). The target is Modern at 60 fps
   on the laptop's Intel U300.
4. **Performance**: laptop A/B runs, a function-level profile, then the
   candidates in the main-thread notes (`r_gpuSync`, ThinLTO, asserts off).
5. **Other platforms**: an aarch64 DXVK build and a first Arm run with game
   data, Windows ARM64 with game data, the macOS client once the renderer
   choice allows it (first run on an Intel MacBook Pro). PS Vita (stretch
   goal): the armv7 dedicated server, then the Vita dedicated server (builds;
   first run on David's Vita next, [guide](vita.md)); a client waits for the
   backend handles (M1b) ([armv7 track](linux-armv7.md)).
6. **Remote play**: Steam login is opt-in (`KISAK_LINUX_STEAM`, PR 85); next
   are a run against a Windows client or server and one with two Steam
   accounts. Admission is never weakened to fake it. HTTP downloads, voice chat
   and the remote script debugger come after.

## Checks before a push

```sh
python3 scripts/tests/test_linux_bootstrap.py
python3 scripts/tests/run_linux_checks.py --build-dir build-linux --binary bin/KisakCOD-dedi
```

Window and renderer changes also run `test_linux_window_workflow.py`,
`test_world_surface_check_runner.py` and `run_world_surface_checks.py` in Debug
and RelWithDebInfo. Client changes run `test_linux_mp_client.py` against the
built client. See [the test guide](../scripts/tests/README.md).
