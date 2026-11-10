# Linux port status

Last updated 2026-11-10 10:57 UTC (master `7eabc4a`, the Linux client's
setup tool, portable folder, movies default and controllers, PR 187, and
the setup tool's first run on the PC's WSL; the first two 64-bit steps in
the upstream patch queue, PR 195). This page is the current state of
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
| Multiplayer client `KisakCOD-mp`, i386 | `-DKISAK_BUILD_LINUX_MP_CLIENT=ON` | Playable on a listen server (laptop): `+devmap mp_crash`, team and class menus, spawn, move, shoot, grenades, OpenAL sound, flashbang white-out, clean quit. All 21 stock maps load, take a player and keep running on i386 and x86_64 in an unattended sweep (laptop, 2026-11-10, master `6e11f2b`: listen server, team and class joined, key presses, one frame measured, a death, clean quit; no assertion, error, script error or dropped connection). Only mp_crash has been played by hand, the sweep's sound went to a silent device, and one of its 42 frames, i386 mp_convoy, was nearly black; a follow-up run is queued ([map sweep](linux-mp-client.md#map-sweep)). Client prediction misses as often as the Windows build (see [Prediction](#prediction)). [Guide](linux-mp-client.md) |
| Multiplayer client, x86_64 | the same, with `-DKISAK_LINUX_ARCH=x86_64` | Playable on the same terms as i386 (laptop), including sprint and mantle animations and a clean quit; all 21 stock maps pass the unattended sweep (see the i386 row). Since PR 187 one command (`scripts/tools/setup_linux_client.py`) fetches and builds the pinned SDL3 and DXVK, with the DXVK fix fullscreen on native Wayland needs, and builds the client; a tool packs it into a folder that starts on another PC with no library path; and `-DKISAK_LINUX_GAMEPAD=ON` (off by default, on in the setup tool) maps a controller to keys and mouse. Hosted CI runs the setup tool end to end on a fresh runner. On the PC's WSL (local, 2026-11-10) it built and packed the client in about 9 minutes, the packed folder played mp_crash with no library path set (Xvfb, software Vulkan), and a second run reused the SDKs and packed a byte-identical client in 5 seconds; it stops if the `--package` folder is not empty. A real GPU and monitor and a real controller in the game are still untried. [Guide](linux-x64.md#x86_64-multiplayer-client) |
| Dedicated server, aarch64 (64-bit Arm) | `-DKISAK_LINUX_ARCH=aarch64` | Builds natively on Arm or cross-compiled from x86, boots without game data (qemu and hosted Arm CI). Not yet run with game data. [Guide](linux-arm64.md) |
| Multiplayer client, aarch64 | the client options with `-DKISAK_LINUX_ARCH=aarch64` | Builds and passes the asset-free client checks (qemu and hosted Arm CI) against a D3D9 stand-in; no aarch64 DXVK build yet, so it has not drawn a frame. [Guide](linux-arm64.md#building-the-aarch64-client) |
| Dedicated server, armv7 (32-bit Arm) | `-DKISAK_LINUX_ARCH=armv7` | Cross-compiled from x86, boots without game data under qemu-arm (hosted CI). First step toward a PS Vita port. Not yet run with game data. [Guide](linux-armv7.md) |
| Dedicated server, PS Vita | `-DKISAK_PLATFORM=vita` | Clang + VitaSDK build packed into `KisakCOD-dedi.vpk` (hosted CI job, artifact). Boots in Vita3K without game data; capped at 24 players so its snapshot buffers fit the Vita's memory. Runs on David's Vita (2026-11-08): run 1 crashed on an unaligned 8-byte store, run 2 loaded mp_crash's zones then crashed on shared thread-locals, run 3 started the game then crashed on an unaligned script vector (all fixed). The run 4 build ran 9 h idle on mp_crash without a crash, and a key-code client on the laptop joined it over Wi-Fi (2026-11-09). An opt-in i386 Linux build (`-DKISAK_ALIGNMENT_CHECK=ON`) lists misaligned accesses ahead of time; the laptop scan of all 21 maps with bots is clean. [Guide](vita.md) |
| Multiplayer client, PS Vita | `-DKISAK_PLATFORM=vita -DKISAK_BUILD_VITA_MP_CLIENT=ON` | KisakCOD-mp compiles, links and packs into `KisakCOD-mp.vpk` (hosted CI job); its own listen server is capped at 8 players. On David's Vita the main menu works (run K), and since console run L (2026-11-10, a build of master with PRs 162, 168, 174 and 175, all since merged) mp_crash loads on the console from the command line and from the menu and quits cleanly, with no GPU memory refused (at most 97.5 of 104 MB). A sceGxm back end under the D3D9 front draws it, the game's shaders are translated to Cg and compiled on the console, and touch, buttons and music work. With the default settings the picture showed double and triple images: the console refuses the depth clear the back end makes for depth of field and soft particles. With `r_dof_enable 0` and `r_zFeather 0` the picture was right and play ran at 5 to 8 frames a second; PR 192 (open) fixes the clear. What made the map fit, each measured first in Vita3K on the PC: render targets get GPU memory at first use and shadow maps are off (PR 155), loaded sounds are compressed to IMA ADPCM (`snd_adpcm`, PR 154; nobody has listened to them), GPU memory is set aside for the colour targets (PR 161), the engine's dynamic buffers take 12.5 MB instead of 25.5 (PR 166), and shader programs are made on a worker thread so the menu no longer freezes for up to 23.6 s while the game loads (PR 164). Sunlit ground is too yellow, a fault of the Vita build in the lightmap term; the flat white sky is the map's own look (PC run Y, beside the Windows build). Since PR 162 the system's on-screen keyboard opens for a menu's text field, the chat line and a typed command; a scripted answer reached the game in Vita3K (PC runs KB1 and KB2), but no text has come back from the system's own dialog and the keyboard has not been tried on the console. The layout of the controls in a game is not confirmed yet. [Guide](vita-client.md) |
| Dedicated server, macOS (arm64 and x86_64) | `-DKISAK_PLATFORM=macos` | Builds with Apple Clang on hosted macOS runners (Apple silicon and Intel) and boots without game data to the first missing fastfile. Not yet run with game data or on a real Mac. [Guide](macos.md) |
| Movies | `KISAK_LINUX_CINEMATICS` (`AUTO`, the default since PR 187) | Bink movies play through an FFmpeg-backed shim on i386 and x86_64 (laptop). Built in when the FFmpeg development files are found at configure time; `OFF` leaves them out, and a build folder made before PR 187 keeps the `OFF` it stored. [Guide](linux-cinematics.md) |
| Steam authentication | `-DKISAK_LINUX_STEAM=ON` | Added by PR 85. Logs in through the running Steam client's `steamclient.so`; a probe got a ticket validated on i386 and x86_64, and an i386 client joined, reconnected to and was refused by (with Steam off) an i386 dedicated server; the same passed with x86_64 client and server, and across i386 and x86_64 in both directions (laptop, one Steam account). A dedicated server can instead log on as an anonymous Steam game server (`KISAK_STEAM_SERVER_LOGIN=anonymous`); a Windows client on David's PC joined such a laptop server and was refused with the server's Steam off. The 32-bit Windows dedicated server can log on the same way since PR 179; that is compiled on hosted CI and has not been run against Steam yet. Off by default. [Guide](linux-steam.md) |
| Key codes (no Steam) | default | A client without Steam (or with `cl_keyCodeOnly 1`) joins servers with `sv_allowUnverifiedGuids 1` using a GUID derived from `codkey.txt` or, on Windows, the retail registry key. Unverified by design. Laptop run (x86_64 client and dedi, made-up key): joined, spawned and reconnected with the GUID; refused without the server opt-in, without a key file, with a group-readable file and with a mistyped key. Since PR 159 the game's own key code menu saves a typed key as `codkey.txt` (mode 600) and keeps it out of the dvars a server's menus can read; that is checked asset-free on hosted CI, and the menu has not been used on screen yet. [Guide](key-codes.md) |
| Finding servers | default | The local list, favourites and `connect` need no setup. The internet list is opt-in: `masterServerName` is empty by default, so no master server is contacted (retail's is gone); when set, the client asks that master and a `dedicated 2` server sends it heartbeats. A list is accepted only from the master that was asked, and since PR 179 the local list shows an unasked reply only when it echoes the random token of the client's last scan and arrives within 10 seconds. Checked asset-free over loopback UDP on hosted CI; no scan of a real network has been run, and the Join Game menu has not been opened on a real client yet. [Guide](server-browser.md) |
| Singleplayer `KisakCOD-sp`, i386 | `-DKISAK_BUILD_LINUX_SP_GAME=ON` | killhouse and cargoship load, leave the pregame by themselves and play about a minute each, clean quit (laptop). All 21 levels load, run 45 s and quit cleanly; a 4-level mission chain and devsave/loadgame pass (laptop sweep run 1, [levels](linux-sp-levels.md)). [Guide](linux-sp-game.md) |
| Vulkan renderer back end | `-DKISAK_BUILD_VULKAN_BACKEND=ON`, then `+set r_backend vulkan` | The game's own D3D9 renderer drawn through this project's Vulkan back end in place of DXVK (Linux) or `d3d9.dll` (Windows), with the original visuals. `KisakCOD-mp` plays mp_crash through it on the laptop (x86_64) and on the PC (the Win32 build from CI), and quits cleanly. The picture matches: the world draw probe's two views agree with DXVK at PSNR 66.7 and 100 (laptop), and on the PC Vulkan and real D3D9 agree at 37 to 44 dB from one indoor view, the same spread as repeats of that comparison. Speed at a fixed mp_crash view on the laptop: since PR 152 (commands recorded on a second thread, a frame handed to the GPU in parts while it is drawn) 140.9 fps against 151.3 through DXVK on mains in power-saver mode (run 9), and 139.5 against 138.2 on battery (run 10, one pass); before it, 116 to 125 against 151 to 159. On the PC every Vulkan launch sat at the display's 75 fps with vsync off, where real D3D9 ran the scene at about 215 (PC run 6; PR 173, open, asks for the IMMEDIATE present mode). Translated shaders and pipelines are kept between runs (PC, Debug build: 15 s to the map warm, 62 s cold). Hosted CI builds it into the i386 Linux client, the macOS client and the Windows game (Win32 and x64), runs the device probe and the adapter and front pixel checks on lavapipe, and runs those two checks on macOS through MoltenVK. The darker models an earlier laptop run showed with `r_altModelLightingUpdate 1` (the default) were the view moving between screenshots, not the back end: measured again over several shots a launch, Vulkan and DXVK on the laptop, and Vulkan and real D3D9 on the PC (PC run 6), draw the same brightness at 0 and at 1, and hosted CI now checks both update paths through the front (PRs 165 and 183). Open: occlusion queries report every sample as passed (PR 173, open, counts them), and about one frame in two minutes takes 110 to 230 ms. Off by default. [Guide](vulkan-d3d9-front.md#in-the-game-r_backend-vulkan) |
| Windows x64 | MSVC `-A x64` (`KISAK_WIN64`) | sp, mp and dedi build and link (CI job "MSVC x64 build"); opt-in FFmpeg movies; OpenAL sound, no Steam. On the PC, mp_crash loads to class select, the dedicated server hosts bots, and singleplayer plays: killhouse and cargoship with autosave and checkpoint loads (PR 95), and a 12-level tour loaded, saved and played without a crash or script error (PRs 111 and 114). A tour of all 21 levels (PR 167's build, 2026-11-10) loaded 20; airlift crashes about 4.5 s in, with the older x64 build too, and PR 193 (open) fixes it (three of three airlift launches held 60 s on the PC). [x64 track](linux-x64.md#windows-msvc-x64-build) |
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
- **Singleplayer without movies.** A `KisakCOD-sp` built without movies
  (`KISAK_LINUX_CINEMATICS` `OFF`, or `AUTO` on a machine without the FFmpeg
  development files) loads no level: the loading screen samples the movie
  images and nothing sets them (found on the laptop, x86_64). PR 193 (open)
  fixes it; build with movies until then.
- **x86_64 Linux singleplayer beyond two levels.** `KisakCOD-sp` builds for
  x86_64 since PR 167. Master's x86_64 build loaded killhouse and airlift
  and quit cleanly in WSL on the PC (software rendering, local runs); PR 193
  (open) says its branch loaded all 21 levels on the laptop
  ([guide](linux-x64.md#x86_64-singleplayer-game)).
- **The PS Vita client's default picture on the console, and its speed.**
  mp_crash loads on the console, but with depth of field and soft particles
  on (the defaults) the picture doubles until PR 192 merges, and play runs
  at 5 to 8 frames a second with both off ([guide](vita-client.md)).
- **A public internet server list**, in any build: the retail master,
  `cod4master.activision.com`, no longer resolves, and no public master
  lists KisakCOD servers. Players find servers by the local list, favourites
  or `connect <address>`. Since PR 163 the master is a setting
  (`masterServerName`, empty by default) for whoever runs one
  ([guide](server-browser.md#the-internet-list)).
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
- **Voice chat and the remote script debugger** are missing from the Linux
  client; each answers as Windows does when that service is absent
  ([list](linux-mp-client.md#what-it-does-not-have-yet)). **HTTP downloads**
  are off by default: `-DKISAK_LINUX_HTTP_DOWNLOADS=ON` builds the engine's
  libcurl downloader in since PR 170, checked against local HTTP and TLS
  servers on hosted CI, but no download from a game server has been made
  end to end yet ([guide](linux-mp-client.md#http-downloads)).

## Security

The 2026-11-01 security audit is closed: PR 18 fixed findings 1 to 3 and PR 106
(2026-11-06) fixed findings 4 to 11, widened the download file name check and
built the Linux games and server with `_FORTIFY_SOURCE=2` and the usual
hardening flags (laptop soak run passed). PR 106 also made CI retry slow apt
mirrors instead of timing out. A daily security check now reads what
merged since its last pass. The first (2026-11-10) read everything merged
since that audit; its fixes are PR 191 (sound size math, the addresses
Windows may open, and the Vita's text run settings) and PR 194 (a Steam
ticket's later verdict), both open.

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

As of 2026-11-10 10:57 UTC, seven. They change by the hour; the
[list on GitHub](https://github.com/koalabuttz/KisakCOD/pulls) is the current
one. A machine run a pull request waits on is named where its author has
said so.

| PR | What | Machine run it waits on |
| --- | --- | --- |
| 173 | Vulkan back end: occlusion queries that count samples, the IMMEDIATE present mode when vsync is off, and GPU feed settings and statistics from the console (`r_vkGpuFeed`, `r_vkStats`) | PC run 7 (is the 75 fps cap gone, the sun against real D3D9) and laptop run 11 (the sun against DXVK, feed settings inside one launch). |
| 191 | Fixes from the first daily security check: sound size math, addresses Windows may open, the Vita's text run settings read once | |
| 192 | PS Vita client: clear depth the way a console accepts, clear the scene's colour every frame, log where a frame's time goes | Vita3K run AA, and a console run with depth of field and soft particles on to prove the fix. |
| 193 | Singleplayer: the 64-bit airlift crash and the movies-off load, and frame and death checks in the level sweep | The i386 sweep of all 21 levels with the new checks, on the laptop. |
| 194 | Steam: a server acts on Steam's later verdict for a ticket | A run with a real Steam player: join, leave and join again, then a kick and an attempt to come back. |
| 197 | Linux: sleep in the waits for the GPU by default (`r_gpuSyncSleep`), and an opt-in to stop them blocking the render thread | |
| 196 | Fewer Linux CI jobs per push: five quick checks run both configurations in one job | |

## Laptop and PC runs still to do

As of 2026-11-10. Each machine does one run at a time. How a run is asked
for, kept short and reported (the `framelog` line, the recipe-driven
frame-rate driver, timed PS Vita starts) is in [runs.md](runs.md), since
PR 168.

- Vulkan back end, PR 173's code: PC run 7 (whether the 75 fps cap is gone,
  the sun against real D3D9) and laptop run 11 (the sun against DXVK, and
  the GPU feed settings compared inside one launch, which run 10 could not
  rank on a falling battery).
- Vulkan probe with the Khronos validation layer (synchronisation checks) on a
  real GPU; the laptop and PC runs so far had no layer installed. The
  standalone Legacy adapter and D3D9 front checks have no recorded PC run
  either (the game itself draws through both there).
- PS Vita client: console run M, PR 192's depth clear with depth of field
  and soft particles on; the keyboard on the console; a listen of the
  compressed sounds; the controls' layout in a game.
- PS Vita dedicated server with bots (it has taken one player).
- Sound listening run for the OpenAL fix (PR 102, merged): plan in
  `sound/laptop-sound-run-1.md`, with `+set snd_alDebug 1`. Needs someone at
  the laptop.
- Singleplayer hand play on the levels the sweep passed (step 6 in
  [linux-sp-levels.md](linux-sp-levels.md)); sweep run 1 is done.
- Multiplayer client: sweep run 1 passed all 21 stock maps on i386 and
  x86_64 ([guide](linux-mp-client.md#map-sweep)). Next: a follow-up on the
  nearly black i386 mp_convoy frame (queued), hand play beyond mp_crash, and
  a 30-minute session on one map. A real HTTP download (PR 170, merged: none has been made end to end).
  The setup tool on a real GPU and monitor (its first run, on the PC's WSL,
  passed with software rendering), and a real controller in the game.
- Finding servers (PR 163, merged): the Join Game menu on a real client, and
  a real dedicated server, the test master and a client together (laptop run
  queued; [guide](server-browser.md#what-is-checked)). A scan of a real
  network for the local list (PR 179, merged; queued on the PC).
- Steam: the Windows dedicated server's anonymous login against Steam
  (PR 179, merged; queued on the PC and the laptop).
- Key code menu (PR 159, merged): a key typed into the menu on screen
  ([guide](key-codes.md#the-key-code-menu)).
- Singleplayer: the sweep's new frame and death checks on i386, all 21
  levels (PR 193 waits on it).
- CPU measuring: laptop perf run 5, the first use of the in-game A/B
  commands, the timed probes and `r_gpuSyncSleep` on a GPU (PR 160, merged;
  [guide](perf-ab.md)).

## Roadmap

The PS Vita client is the stated priority; the other tracks run beside it, in
no fixed order.

1. **Land the open work above**.
2. **PS Vita client** (the priority since 2026-11-09): the menu and
   mp_crash run on the console (console run L), with the dynamic GPU
   buffers cut to fit (PR 166) and shader programs made in the background
   so the menu does not freeze (PR 164). Next is the console's depth clear
   (PR 192), speed, text typed on the console's own keyboard (PR 162 is
   merged and checked in the emulator), and joining a server
   ([guide](vita-client.md#stages)).
3. **Renderer**: the Vulkan back end with the original visuals draws the game
   on the laptop and the PC (PRs 124 to 152), on the laptop now near DXVK's
   speed. Next is the PC's 75 fps cap and occlusion queries (PR 173), the
   render thread's time per draw, the open differences in the table above,
   and the checks on more GPUs. The Modern path comes after (plan
   in the project files, `renderer/modern-renderer-plan.md`). The target is
   Modern at 60 fps on the laptop's Intel U300.
4. **Linux client polish**: hand play on the other maps (all 21 pass the
   unattended sweep), a long session, a first real HTTP download (the
   opt-in downloader is merged, PR 170), then voice chat and the remote
   script debugger
   ([list](linux-mp-client.md#what-it-does-not-have-yet)).
5. **Linux singleplayer beyond two levels**: hand play of the swept levels,
   and the x86_64 and movies-off fixes (PR 193).
6. **Performance on the CPU**: the laptop A/B runs and the function-level
   profile are done (PRs 97 and 112). Their reading is that the laptop is
   limited by its GPU at both sizes measured, so CPU work there is about
   power and long frames. Measuring inside one launch is merged (`perf_ab`,
   PR 160, [guide](perf-ab.md)), because the laptop's speed drifts more
   between launches than most changes are worth; its first run on the laptop
   is next. One switch it brought, `xanim_blendOnce` (off by default),
   calculated 131072 animation blends both ways on the PC with none
   different.
7. **Remote play**: Steam login is opt-in (`KISAK_LINUX_STEAM`, PR 85); a
   Windows client joined a Linux server logged on as an anonymous game server
   (PR 133), and a client without Steam joins servers that accept key code
   GUIDs (PR 134). Next: a Linux Steam client on an anonymous-login server,
   a Windows key code client, a Steam player and a key code player on one
   server, two Steam accounts, and the internet list seen working in the
   Join Game menu (PR 163 made the master a setting).
   Admission is never weakened to fake it.
8. **Other platforms**: an aarch64 DXVK build and a first Arm run with game
   data, Windows ARM64 with game data, the macOS client with game data
   through `r_backend vulkan` (first run on an Intel MacBook Pro; a release
   bundle also needs MoltenVK and the Vulkan loader), and bots on the PS Vita
   dedicated server ([guide](vita.md)).
9. **Security**: the daily check's first fixes (PRs 191 and 194).

## Checks before a push

```sh
python3 scripts/tests/test_linux_bootstrap.py
python3 scripts/tests/run_linux_checks.py --build-dir build-linux --binary bin/KisakCOD-dedi
```

What hosted CI then runs is in [ci.md](ci.md). Since PR 168 a draft pull
request runs nothing, a pull request runs only the workflows its changed
paths can reach, and MSVC x64, MSVC ARM64 and Intel macOS build after the
merge to master, not on the pull request.

Window and renderer changes also run `test_linux_window_workflow.py`,
`test_world_surface_check_runner.py` and `run_world_surface_checks.py` in Debug
and RelWithDebInfo. Client changes run `test_linux_mp_client.py` against the
built client. See [the test guide](../scripts/tests/README.md).
