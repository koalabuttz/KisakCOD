# Linux port status

Last updated 2026-11-05 (master `7160b20` plus the arm64 dedicated server). This page is the current state of
the native Linux port. The [docs index](README.md) lists the detailed guides;
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
| Multiplayer client `KisakCOD-mp`, i386 | `-DKISAK_BUILD_LINUX_MP_CLIENT=ON` | Playable on a listen server (laptop): `+devmap mp_crash`, team and class menus, spawn, move, shoot, grenades, OpenAL sound, flashbang white-out, clean quit. [Guide](linux-mp-client.md) |
| Multiplayer client, x86_64 | the same, with `-DKISAK_LINUX_ARCH=x86_64` | Playable on the same terms as i386 (laptop), including sprint and mantle animations and a clean quit. [Guide](linux-x64.md#x86_64-multiplayer-client) |
| Dedicated server, aarch64 (64-bit Arm) | `-DKISAK_LINUX_ARCH=aarch64` | Builds natively on Arm or cross-compiled from x86, boots without game data (qemu and hosted Arm CI). Not yet run with game data. [Guide](linux-arm64.md) |
| Movies | `-DKISAK_LINUX_CINEMATICS=ON` | Bink movies play through an FFmpeg-backed shim on i386 and x86_64 (laptop). Off by default. [Guide](linux-cinematics.md) |
| Singleplayer `KisakCOD-sp`, i386 | `-DKISAK_BUILD_LINUX_SP_GAME=ON` | killhouse and cargoship load, leave the pregame by themselves and play about a minute each, clean quit (laptop). [Guide](linux-sp-game.md) |

The client builds need DXVK Native, SDL3 3.2.10 or newer and OpenAL for the
target architecture; the guides above give the CMake lines. The game files
always come from your own install (`+set fs_basepath <game folder>`).

## What does not work yet

- **Joining a remote server.** Linux has no Steam ticket provider, so the client
  refuses to send an unauthenticated challenge, and the dedicated server rejects
  ordinary client tickets unless an explicit opt-in is set
  ([details](linux-dedicated.md#local-connection-testing-without-steam-authentication)).
  This stays refused until a real provider exists.
- **Prediction.** The Linux client logs about five times as many prediction
  errors and misses as the Windows build does over the same kind of play
  (laptop vs a Windows PC, 2026-11-04). Being investigated.
- **x86_64 singleplayer.** The 64-bit configuration refuses the SP game for now.
- **Windows x64.** The MSVC x64 compile census runs on master
  ([census](linux-x64.md#windows-msvc-x64-census)); the first compile fixes
  are in review (PR 81). No 64-bit Windows binary links yet.
- **arm64 client and Windows ARM64.** Census only so far
  ([arm64 track](linux-arm64.md#census-linux-client-step-3)); the aarch64
  configuration builds the dedicated server and refuses the other targets.
- **HTTP downloads, voice chat and the remote script debugger** are missing from
  the Linux client; each answers as Windows does when that service is absent
  ([list](linux-mp-client.md#what-it-does-not-have-yet)).

## Other Linux pieces

These came before the playable client and still build and run in CI:

- The [free camera](linux-free-camera.md) and the
  [world draw probe](linux-world-draw-probe.md) fly and draw a map through the
  engine renderer without a player.
- The [client core](linux-client.md) test targets compile client translation
  units into small asset-free tests.
- The [standalone renderer](../tests/linux_renderer/README.md) replays single
  exported world surfaces and checks pixels against CPU predictions.

## Checks before a push

```sh
python3 scripts/tests/test_linux_bootstrap.py
python3 scripts/tests/run_linux_checks.py --build-dir build-linux --binary bin/KisakCOD-dedi
```

Window and renderer changes also run `test_linux_window_workflow.py`,
`test_world_surface_check_runner.py` and `run_world_surface_checks.py` in Debug
and RelWithDebInfo. Client changes run `test_linux_mp_client.py` against the
built client. See [the test guide](../scripts/tests/README.md).
