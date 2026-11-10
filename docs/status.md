# Linux port status

Last updated 2026-11-10 (master `ac06197`, the PS Vita client's compressed
sounds, PR 154). This page is the current state of
the native Linux port and the other platform builds, the open pull requests and
the roadmap. The [docs index](README.md) lists the detailed guides;
older handoffs are in [history](history/README.md) and are not current.

Results marked *laptop* come from one developer machine (Intel Raptor Lake-U,
Mesa ANV, DXVK Native 2.7.1, SDL 3.2.10, OpenAL Soft) with the game files
installed. Results marked *PC* come from one Windows desktop (RTX 2080 Super),
*console* from one PS Vita, and *Vita3K* from that emulator on the laptop or
the PC. None of them is CI evidence. Hosted CI builds and runs everything below
without a GPU or game data, against stand-in D3D9 and OpenAL libraries (the
Vulkan checks run on lavapipe and MoltenVK).

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
| Dedicated server, PS Vita | `-DKISAK_PLATFORM=vita` | Clang + VitaSDK build packed into `KisakCOD-dedi.vpk` (hosted CI job, artifact). Boots in Vita3K without game data; capped at 24 players so its snapshot buffers fit the Vita's memory. Runs on David's Vita (2026-11-08): run 1 crashed on an unaligned 8-byte store, run 2 loaded mp_crash's zones then crashed on shared thread-locals, run 3 started the game then crashed on an unaligned script vector (all fixed). The run 4 build ran 9 h idle on mp_crash without a crash, and a key-code client on the laptop joined it over Wi-Fi (2026-11-09). An opt-in i386 Linux build (`-DKISAK_ALIGNMENT_CHECK=ON`) lists misaligned accesses ahead of time; the laptop scan of all 21 maps with bots is clean. [Guide](vita.md) |
| Multiplayer client, PS Vita | `-DKISAK_PLATFORM=vita -DKISAK_BUILD_VITA_MP_CLIENT=ON` | KisakCOD-mp compiles, links and packs into `KisakCOD-mp.vpk` (hosted CI job); its own listen server is capped at 8 players. On David's Vita the main menu shows and works (run K, 2026-11-10): a sceGxm back end under the D3D9 front draws it, the game's 826 shaders are translated to Cg and compiled on the console, and touch, buttons and the menu's music work. A map does not load yet, and memory is the work in progress. On the console (run K) the menu filled 95 of the GPU's 104 MB and 55 of the 64 MB heap, most of the heap with sounds held as 16-bit PCM, and a listen server on mp_crash ran out of GPU memory. Two merged changes cut both, measured so far only in Vita3K on the PC. PR 155 gives render targets their GPU memory at first use and turns shadow maps off: the menu holds 71 of 108 MB, and the whole mp_crash fastfile loads before the heap runs out. PR 154 compresses loaded sounds to IMA ADPCM when they load (`snd_adpcm`, the Vita's default): the menu's heap is at 24 MB. The two have not run together, neither has run on the console, and nobody has listened to the compressed sounds. With a 128 MB heap, which a console does not have, and without PR 154, the map loaded and a match ran in Vita3K with menus and HUD drawn, but the world was black. The layout of the controls in a game is not confirmed yet. [Guide](vita-client.md) |
| Dedicated server, macOS (arm64 and x86_64) | `-DKISAK_PLATFORM=macos` | Builds with Apple Clang on hosted macOS runners (Apple silicon and Intel) and boots without game data to the first missing fastfile. Not yet run with game data or on a real Mac. [Guide](macos.md) |
| Movies | `-DKISAK_LINUX_CINEMATICS=ON` | Bink movies play through an FFmpeg-backed shim on i386 and x86_64 (laptop). Off by default. [Guide](linux-cinematics.md) |
| Steam authentication | `-DKISAK_LINUX_STEAM=ON` | Added by PR 85. Logs in through the running Steam client's `steamclient.so`; a probe got a ticket validated on i386 and x86_64, and an i386 client joined, reconnected to and was refused by (with Steam off) an i386 dedicated server; the same passed with x86_64 client and server, and across i386 and x86_64 in both directions (laptop, one Steam account). A dedicated server can instead log on as an anonymous Steam game server (`KISAK_STEAM_SERVER_LOGIN=anonymous`); a Windows client on David's PC joined such a laptop server and was refused with the server's Steam off. Off by default. [Guide](linux-steam.md) |
| Key codes (no Steam) | default | A client without Steam (or with `cl_keyCodeOnly 1`) joins servers with `sv_allowUnverifiedGuids 1` using a GUID derived from `codkey.txt` or, on Windows, the retail registry key. Unverified by design. Laptop run (x86_64 client and dedi, made-up key): joined, spawned and reconnected with the GUID; refused without the server opt-in, without a key file, with a group-readable file and with a mistyped key. [Guide](key-codes.md) |
| Singleplayer `KisakCOD-sp`, i386 | `-DKISAK_BUILD_LINUX_SP_GAME=ON` | killhouse and cargoship load, leave the pregame by themselves and play about a minute each, clean quit (laptop). All 21 levels load, run 45 s and quit cleanly; a 4-level mission chain and devsave/loadgame pass (laptop sweep run 1, [levels](linux-sp-levels.md)). [Guide](linux-sp-game.md) |
| Vulkan renderer back end | `-DKISAK_BUILD_VULKAN_BACKEND=ON`, then `+set r_backend vulkan` | The game's own D3D9 renderer drawn through this project's Vulkan back end in place of DXVK (Linux) or `d3d9.dll` (Windows), with the original visuals. `KisakCOD-mp` plays mp_crash through it on the laptop (x86_64) and on the PC (the Win32 build from CI), and quits cleanly. The picture matches: the world draw probe's two views agree with DXVK at PSNR 66.7 and 100 (laptop), and on the PC Vulkan and real D3D9 agree at 37 to 44 dB from one indoor view, the same spread as repeats of that comparison. Speed on master: 116 to 125 fps against 151 to 159 through DXVK at a fixed mp_crash view (laptop, on battery); PR 152 (open) measured 141 against 151 to 155. Translated shaders and pipelines are kept between runs (PC, Debug build: 15 s to the map warm, 62 s cold). Hosted CI builds it into the i386 Linux client, the macOS client and the Windows game (Win32 and x64), runs the device probe and the adapter and front pixel checks on lavapipe, and runs those two checks on macOS through MoltenVK. Open: models are slightly darker than through DXVK with `r_altModelLightingUpdate 1` (the default), occlusion queries report every sample as passed, and about one frame in two minutes takes 110 to 230 ms. Off by default. [Guide](vulkan-d3d9-front.md#in-the-game-r_backend-vulkan) |
| Windows x64 | MSVC `-A x64` (`KISAK_WIN64`) | sp, mp and dedi build and link (CI job "MSVC x64 build"); opt-in FFmpeg movies; OpenAL sound, no Steam. On the PC, mp_crash loads to class select, the dedicated server hosts bots, and singleplayer plays: killhouse and cargoship with autosave and checkpoint loads (PR 95), and a 12-level tour loaded, saved and played without a crash or script error (PRs 111 and 114). [x64 track](linux-x64.md#windows-msvc-x64-build) |
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

- **Joining a Steam-only server from the default build.** It has no Steam
  ticket provider; it joins only servers that accept key code GUIDs
  (`sv_allowUnverifiedGuids 1`, [key codes](key-codes.md)). Build with `-DKISAK_LINUX_STEAM=ON` to authenticate through the
  Steam client running on the machine ([guide](linux-steam.md)). A Windows
  client has joined a Linux server that way. Not yet tried: a Windows server,
  two Steam accounts.
- **Singleplayer played by hand past two levels.** All 21 levels load and run
  unattended, and mission changes and save loading work from the console
  ([linux-sp-levels.md](linux-sp-levels.md)). Only killhouse and cargoship
  have been played by hand; night vision, ac130, the Javelin, slow motion,
  death and retry, and the victory screen are untried.
- **x86_64 Linux singleplayer.** The 64-bit Linux configuration refuses the SP
  game for now (Windows x64 singleplayer plays, see the table).
- **A map on the PS Vita client.** The menu works on the console; a map does
  not fit in memory yet ([guide](vita-client.md#memory)).
- **macOS client.** The multiplayer client builds for Macs, with the Bink movie
  shim and the Vulkan back end (`r_backend vulkan`) linked in. It passes its
  asset-free checks in hosted CI against the D3D9 and OpenAL stand-ins. The
  Vulkan back end's own checks (shader translator, Legacy adapter, D3D9 front)
  pass on Metal through MoltenVK on both hosted Mac runners (PR 136, which also
  worked around three MoltenVK gaps). A universal app bundle is about 8 MiB
  zipped. Not yet run with game data or on a real Mac
  ([macOS track](macos.md#vulkan-back-end)).
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
mirrors instead of timing out. Code added since that audit (the Vulkan back
end, the PS Vita port, key code joins, the anonymous Steam server login) has
not had an audit pass of its own yet.

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
  paths and check its pixels. It is the base of the Vulkan back end in the
  table above. Hosted CI runs it on lavapipe (Linux) and
  MoltenVK (macOS); the clear and draw probes also ran on the laptop GPU, the
  clear probe on the PC. On top of it, the
  [Legacy shader translator](vulkan-legacy-shaders.md) and the
  [Legacy adapter](vulkan-legacy-adapter.md) (`-DKISAK_BUILD_VULKAN_LEGACY=ON`),
  the part of D3D9 the renderer uses, with its own pixel check, and its
  [D3D9 front](vulkan-d3d9-front.md), the D3D9 interfaces on the adapter with
  a check of its own; hosted CI runs both on lavapipe, and both passed on the
  laptop GPU. With
  `-DKISAK_BUILD_VULKAN_BACKEND=ON` the device probe, the world draw probe and
  `KisakCOD-mp` link it, and `r_backend vulkan` draws the renderer through it.

## Open pull requests

As of 2026-11-10.

| PR | What | Waits on |
| --- | --- | --- |
| 152 | Vulkan back end: record commands on a second thread, as DXVK does, and submit a frame to the GPU in parts while it is drawn ([front](vulkan-d3d9-front.md#threads)) | Hosted CI is green. Laptop run 9 measured 141 fps against 151 to 155 through DXVK with an unchanged picture; a run of the feed interval on the laptop and a first run on the PC are queued. |

## Laptop and PC runs still to do

As of 2026-11-10. Each machine does one run at a time.

- Vulkan back end, PR 152: the feed interval on the laptop (run 10), and the
  same code on the PC (PC run 6), which also compares real D3D9 with
  `r_altModelLightingUpdate` 0 and 1 from one view to settle the model
  lighting difference.
- Vulkan probe with the Khronos validation layer (synchronisation checks) on a
  real GPU; the laptop and PC runs so far had no layer installed. The
  standalone Legacy adapter and D3D9 front checks have no recorded PC run
  either (the game itself draws through both there).
- PS Vita client: master, with PRs 154 and 155 together, in Vita3K and then
  on the console, to see whether mp_crash fits the 64 MB heap; the black world
  seen in Vita3K; a listen of the compressed sounds; the controls' layout in a
  game.
- PS Vita dedicated server with bots (it has taken one player).
- Sound listening run for the OpenAL fix (PR 102, merged): plan in
  `sound/laptop-sound-run-1.md`, with `+set snd_alDebug 1`. Needs someone at
  the laptop.
- Singleplayer hand play on the levels the sweep passed (step 6 in
  [linux-sp-levels.md](linux-sp-levels.md)); sweep run 1 is done.
- Multiplayer client beyond mp_crash: the other stock maps and a long session,
  i386 and x86_64. Only mp_crash has been played.

## Roadmap

The PS Vita client is the stated priority; the other tracks run beside it, in
no fixed order.

1. **Land the open work above**.
2. **PS Vita client** (the priority since 2026-11-09): the menu runs on the
   console. Next is a map that fits the Vita's memory (the dynamic GPU buffers
   are still sized for a PC), then the black world, the on-screen keyboard, joining a
   server, and speed ([guide](vita-client.md#stages)).
3. **Renderer**: the Vulkan back end with the original visuals draws the game
   on the laptop and the PC (PRs 124 to 148). Next is its speed against DXVK
   (PR 152, then render-thread time per draw), the open differences in the
   table above, and the checks on more GPUs. The Modern path comes after (plan
   in the project files, `renderer/modern-renderer-plan.md`). The target is
   Modern at 60 fps on the laptop's Intel U300.
4. **Linux client polish**: the other maps, a long session, HTTP downloads,
   then voice chat and the remote script debugger
   ([list](linux-mp-client.md#what-it-does-not-have-yet)).
5. **Linux singleplayer beyond two levels**: hand play of the swept levels,
   then x86_64 singleplayer.
6. **Performance on the CPU**: laptop A/B runs, a function-level profile, then
   the candidates in the main-thread notes (`r_gpuSync`, ThinLTO, asserts off).
7. **Remote play**: Steam login is opt-in (`KISAK_LINUX_STEAM`, PR 85); a
   Windows client joined a Linux server logged on as an anonymous game server
   (PR 133), and a client without Steam joins servers that accept key code
   GUIDs (PR 134). Next: a Windows server, a run with two Steam accounts, a
   real key, the server browser. Admission is never weakened to fake it.
8. **Other platforms**: an aarch64 DXVK build and a first Arm run with game
   data, Windows ARM64 with game data, the macOS client with game data
   through `r_backend vulkan` (first run on an Intel MacBook Pro; a release
   bundle also needs MoltenVK and the Vulkan loader), and bots on the PS Vita
   dedicated server ([guide](vita.md)).
9. **Security**: an audit pass over the code added since 2026-11-01.

## Checks before a push

```sh
python3 scripts/tests/test_linux_bootstrap.py
python3 scripts/tests/run_linux_checks.py --build-dir build-linux --binary bin/KisakCOD-dedi
```

Window and renderer changes also run `test_linux_window_workflow.py`,
`test_world_surface_check_runner.py` and `run_world_surface_checks.py` in Debug
and RelWithDebInfo. Client changes run `test_linux_mp_client.py` against the
built client. See [the test guide](../scripts/tests/README.md).
