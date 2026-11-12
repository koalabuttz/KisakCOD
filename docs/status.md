# Linux port status

Last updated 2026-11-12 04:12 UTC (master `205189b`: Vulkan fullscreen applies the gamma ramp,
switches the display mode and survives alt-tab, PR 252; Steam is opt-in for
the multiplayer client, PR 255; Vulkan pipelines built ahead, PR 237; a
Field of View slider, PR 248; Windows parity fixes, PRs 246 and 247; a server
can no longer set the settings this port added on a joining client, PR 225;
temporal anti-aliasing on the Vulkan back end, PR 245; the Vita draws the
scene's depth first, PR 253). This page is the current state of
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
| Multiplayer client `KisakCOD-mp`, i386 | `-DKISAK_BUILD_LINUX_MP_CLIENT=ON` | Playable on a listen server (laptop): `+devmap mp_crash`, team and class menus, spawn, move, shoot, grenades, OpenAL sound, flashbang white-out, clean quit. All 21 stock maps load, take a player and keep running on i386 and x86_64 in an unattended sweep (laptop, 2026-11-10, master `6e11f2b`: listen server, team and class joined, key presses, one frame measured, a death, clean quit; no assertion, error, script error or dropped connection). Only mp_crash has been played by hand, the sweep's sound went to a silent device, and one of its 42 frames, i386 mp_convoy, was nearly black; a follow-up run is queued ([map sweep](linux-mp-client.md#map-sweep)). Client prediction misses as often as the Windows build (see [Prediction](#prediction)). Windows parity fixes from the audit (PRs 246 and 247, merged; checked locally by their authors): the Linux, macOS and Vita ports read `localization.txt` and load `zone/<language>/` where they always loaded english; keypad digits type as text only in the console and menus; the Linux and macOS clients have a clipboard through SDL; the real D3D9 or DXVK adapter name reaches autoconfigure; `screenshotJpeg` and the singleplayer save-game shot write JPEGs off Windows (stb_image_write in `deps/stb`); file and folder lookups ignore case; and letter and punctuation keys follow the keyboard layout (the number row and the console key stay positional). A non-US keyboard has not been tried. Options > Game has a Field of View slider (PR 248, merged; 65 to 120 on every server, built at run time over the game's own menu rows; checked offline on a retail `ui_mp.ff` only, so nobody has seen it in a running client, and a PC run in WSL is being queued). PR 258 (merged) fixes a counter of the mark vertex generation that was raised in builds without assertions but lowered only inside `iassert`, which stopped laptop perf run 6's speed build twice on mp_crash 10 to 15 s after the map load; no game has run with the fix yet. [Guide](linux-mp-client.md) |
| Multiplayer client, x86_64 | the same, with `-DKISAK_LINUX_ARCH=x86_64` | Playable on the same terms as i386 (laptop), including sprint and mantle animations and a clean quit; all 21 stock maps pass the unattended sweep (see the i386 row). Since PR 187 one command (`scripts/tools/setup_linux_client.py`) fetches and builds the pinned SDL3 and DXVK, with the DXVK fix fullscreen on native Wayland needs, and builds the client; a tool packs it into a folder that starts on another PC with no library path; and `-DKISAK_LINUX_GAMEPAD=ON` (off by default, on in the setup tool) maps a controller to keys and mouse. Hosted CI runs the setup tool end to end on a fresh runner. On the PC's WSL (local, 2026-11-10) it built and packed the client in about 9 minutes, the packed folder played mp_crash with no library path set (Xvfb, software Vulkan), and a second run reused the SDKs and packed a byte-identical client in 5 seconds; it stops if the `--package` folder is not empty. A real GPU and monitor and a real controller in the game are still untried. [Guide](linux-x64.md#x86_64-multiplayer-client) |
| Dedicated server, aarch64 (64-bit Arm) | `-DKISAK_LINUX_ARCH=aarch64` | Builds natively on Arm or cross-compiled from x86, boots without game data (qemu and hosted Arm CI). Not yet run with game data. [Guide](linux-arm64.md) |
| Multiplayer client, aarch64 | the client options with `-DKISAK_LINUX_ARCH=aarch64` | Builds and passes the asset-free client checks (qemu and hosted Arm CI) against a D3D9 stand-in; no aarch64 DXVK build yet, so it has not drawn a frame. [Guide](linux-arm64.md#building-the-aarch64-client) |
| Dedicated server, armv7 (32-bit Arm) | `-DKISAK_LINUX_ARCH=armv7` | Cross-compiled from x86, boots without game data under qemu-arm (hosted CI). First step toward a PS Vita port. Not yet run with game data. [Guide](linux-armv7.md) |
| Dedicated server, PS Vita | `-DKISAK_PLATFORM=vita` | Clang + VitaSDK build packed into `KisakCOD-dedi.vpk` (hosted CI job, artifact). Boots in Vita3K without game data; capped at 24 players so its snapshot buffers fit the Vita's memory. Runs on David's Vita (2026-11-08): run 1 crashed on an unaligned 8-byte store, run 2 loaded mp_crash's zones then crashed on shared thread-locals, run 3 started the game then crashed on an unaligned script vector (all fixed). The run 4 build ran 9 h idle on mp_crash without a crash, and a key-code client on the laptop joined it over Wi-Fi (2026-11-09). An opt-in i386 Linux build (`-DKISAK_ALIGNMENT_CHECK=ON`) lists misaligned accesses ahead of time; the laptop scan of all 21 maps with bots is clean. [Guide](vita.md) |
| Multiplayer client, PS Vita | `-DKISAK_PLATFORM=vita -DKISAK_BUILD_VITA_MP_CLIENT=ON` | KisakCOD-mp compiles, links and packs into `KisakCOD-mp.vpk` (hosted CI job); its own listen server is capped at 8 players. On David's Vita the main menu works (run K), and since console run L (2026-11-10, a build of master with PRs 162, 168, 174 and 175, all since merged) mp_crash loads on the console from the command line and from the menu and quits cleanly, with no GPU memory refused (at most 97.5 of 104 MB). A sceGxm back end under the D3D9 front draws it, the game's shaders are translated to Cg and compiled on the console, and touch, buttons and music work. With the default settings the picture showed double and triple images: the console refuses the depth clear the back end makes for depth of field and soft particles. With `r_dof_enable 0` and `r_zFeather 0` the picture was right and play ran at 5 to 8 frames a second. PR 192 (merged) clears depth in a way the console accepts and clears the scene's colour every frame; in Vita3K its pictures match run Y with no smear (PC run AA), and in console run N (below) David saw the default picture, with depth of field and soft particles on, as normal. What made the map fit, each measured first in Vita3K on the PC: render targets get GPU memory at first use and shadow maps are off (PR 155), loaded sounds are compressed to IMA ADPCM (`snd_adpcm`, PR 154; nobody has listened to them), GPU memory is set aside for the colour targets (PR 161), the engine's dynamic buffers take 12.5 MB instead of 25.5 (PR 166), and shader programs are made on a worker thread so the menu no longer freezes for up to 23.6 s while the game loads (PR 164). Since PR 202 static models in view go out in one draw again from a 4 MB cache, and the back end skips program lookups, texture binds and uniform buffers a draw repeats (a `no-draw-reuse` file turns that off): in Vita3K the ground view takes 30.2 ms a frame, against 47.2 with the savings off and 67.0 with no cache (PC run AC). Since PR 212 retired GPU memory is freed on a map change, so mp_backlot loads after mp_crash in Vita3K (run Z9). In a Vita3K sweep of all 21 maps (a local build of master with three more commits) every map started, loaded and quit cleanly; mp_carentan ran out of GPU memory and only mp_killhouse reached 60 fps. On the console (run N, 2026-11-10, a build of master c94ea9d with PRs 192 and 202, default effects on): David wrote that it looked normal on the screen, both with the per-draw savings and with them off by `no-draw-reuse`; no GPU memory was refused (at most 99.5 of 104 MB), no draw was skipped and there was no error. The back end's draw time fell from 28 to 30 ms a frame with the savings off to 13 to 17 ms with them on, but whole frames stayed at 133 to 165 ms (6 to 7 frames a second) facing the busy way, because the GPU's wait is 78 to 113 ms of that; turned away they ran at about 67 to 70 ms (14 to 15 frames a second), all of it the GPU's 51 ms. Sunlit ground is too yellow, a fault of the Vita build in the lightmap term; the flat white sky is the map's own look (PC run Y, beside the Windows build). Since PR 211 shader constants written as literals compile right, so the wall posters match Windows in Vita3K (run Z8); the ground under decals still shows a red and green speckle, which PR 214 (open) removes by building the fragment programs that sample a cube map (`texCUBElod`) without fast maths (run Z10, two programs); building every fragment program that way made the console's shader compiler fail (run Z11). Since PR 253 (merged) the scene's depth is always drawn before its lit pass on the Vita (`no-depth-prepass`, an empty file in the program folder, turns it off), so the GPU skips hidden pixels early, and the view model's depth range is a sliver just past its own so the gun no longer shows black: console runs S and T found the GPU wait on the busy way fell from 108 to 80 ms and the other way from 50 to 27 ms with `r_depthPrepass 1`; console run U (2026-11-11, a build of PR 253's head with the pass forced) measured 85.5 to 92.4 ms busy and 34.5 to 34.9 ms the other way (about 20 fps), and David wrote that the picture and the gun looked normal in every view, including after the turn (his words, not a picture check); PC run Z17b in Vita3K matched the picture with the pass off. Run U's draws left out while their program was made counted 18,541 over 47 frames, more than runs S1 and T (the log does not say in which frames). Console run V counted seven scenes a frame at the busy view (the back buffer four times, the forced depth pass once, two copies of the picture with no draws at 2.25 ms each), and a view of 60 unlit draws still cost the GPU 22 ms. Since PR 162 the system's on-screen keyboard opens for a menu's text field, the chat line and a typed command; a scripted answer reached the game in Vita3K (PC runs KB1 and KB2), but no text has come back from the system's own dialog and the keyboard has not been tried on the console. The layout of the controls in a game is not confirmed yet. [Guide](vita-client.md) |
| Dedicated server, macOS (arm64 and x86_64) | `-DKISAK_PLATFORM=macos` | Builds with Apple Clang on hosted macOS runners (Apple silicon and Intel) and boots without game data to the first missing fastfile. Since PR 223 (merged) each architecture's two configurations build in one job. Not yet run with game data or on a real Mac. [Guide](macos.md) |
| Movies | `KISAK_LINUX_CINEMATICS` (`AUTO`, the default since PR 187) | Bink movies play through an FFmpeg-backed shim on i386 and x86_64 (laptop). Built in when the FFmpeg development files are found at configure time; `OFF` leaves them out, and a build folder made before PR 187 keeps the `OFF` it stored. [Guide](linux-cinematics.md) |
| Steam authentication | `-DKISAK_LINUX_STEAM=ON` | Added by PR 85. Since PR 255 (merged, 2026-11-12) the multiplayer client, on Windows and on Linux Steam builds alike, uses Steam only when started with `+set cl_steam 1`: without it the client never calls `SteamAPI_Init` or opens `steamclient.so`, writes no `steam_appid.txt`, prints `[Kisak] Steam is off, so remote servers are joined with your key code.` and joins with its key code; Windows players who use Steam now need `+set cl_steam 1`. In Steam opt-in run 1 (the PC, a local MSVC Win32 Release build of PR 255's head, not CI) the client without `cl_steam`, and with `+set cl_steam 0`, joined a Win32 dedicated server as the key code GUID with no `SteamAPI_Init` line and no `steam_appid.txt`; `cl_steam 1` itself was not tried there, since Steam was never started. The dedicated server and singleplayer are unchanged: the Win32 dedicated server still calls `SteamAPI_Init` at start-up and writes a `steam_appid.txt` into its working directory. A KisakCOD player identity that would let default servers admit non-Steam players is on hold by David's choice. The runs below were made before PR 255, with Steam on by default. Logs in through the running Steam client's `steamclient.so`; a probe got a ticket validated on i386 and x86_64, and an i386 client joined, reconnected to and was refused by (with Steam off) an i386 dedicated server; the same passed with x86_64 client and server, and across i386 and x86_64 in both directions (laptop, one Steam account). A dedicated server can instead log on as an anonymous Steam game server (`KISAK_STEAM_SERVER_LOGIN=anonymous`); a Windows client on David's PC joined such a laptop server and was refused with the server's Steam off. The 32-bit Windows dedicated server can log on the same way since PR 179: in Steam Windows run 3 (2026-11-10, a local Release build on the PC) it logged on anonymously, the laptop's x86_64 Linux Steam client joined it over the LAN twice and passed the Steam auth check, and with Steam off on the server the client was refused. A server acts on Steam's later verdict for a ticket (PR 194: a dropped player needs a new ticket; Steam verdict run 1 passed with Linux and Windows servers), and in mixed session run 1 (laptop, 2026-11-11, local builds of master `eb316f3`, not CI) one anonymous-login Steam dedicated server held a Steam player and a key code player at once: each left and rejoined without dropping the other, `steam_testkick` dropped only the Steam player, who rejoined, and every program exited 0. The kicked client's popup is checked by its dvars, not a screenshot: `com_errorTitle` "Notice" and `com_errorMessage` "<player name> Steam Auth Timed out". It accepts a SteamID64 in one spelling only so a ban covers the player, and the Windows server's Steam start-up lines print after start-up (PR 216, not yet seen on Windows). Off by default. [Guide](linux-steam.md) |
| Key codes (no Steam) | default | A client without Steam (or with `cl_keyCodeOnly 1`, or since PR 255 without `+set cl_steam 1`) joins servers with `sv_allowUnverifiedGuids 1` using a GUID derived from `codkey.txt` or, on Windows, the retail registry key. Unverified by design. Laptop run (x86_64 client and dedi, made-up key): joined, spawned and reconnected with the GUID; refused without the server opt-in, without a key file, with a group-readable file and with a mistyped key. Since PR 159 the game's own key code menu saves a typed key as `codkey.txt` (mode 600) and keeps it out of the dvars a server's menus can read; that is checked asset-free on hosted CI. A script that clicks through the menu under Xvfb (`scripts/runs/menu_check.py`, PR 216) refused a mistyped key and saved the made-up test key with mode 600 in menu check run 9 on the PC's WSL, with software rendering; nobody has used the menu on a real screen. Windows key code run 1 (2026-11-11, the PC, local MSVC builds of master `f9506d9`, not CI): a Win32 client with only a key code (`codkey.txt`, `cl_keyCodeOnly 1`, no Steam) joined and rejoined a Win32 dedicated server and the x86_64 Linux dedicated server in WSL, both with `sv_allowUnverifiedGuids 1`, as the expected GUID; it was refused by a server without that setting, with no key file and with a mistyped key, and the key text appeared in no log. The Windows Firewall prompt for the Linux server run was answered at the PC. Listen server run 1 (the PC, local builds of the same commit, not CI) hosted a game from the x86_64 Linux `KisakCOD-mp` in WSL with `sv_allowUnverifiedGuids 1`: its own player stayed in the game while a Win32 client (over the WSL address) and a second Linux client (on 127.0.0.1) each joined and rejoined on the key code alone; without the setting the joiner was refused and the host carried on. In those runs (before PR 242) neither the dedicated nor the listen server logged anything about a refused joiner; since PR 242 (merged) a server that refuses a `getchallenge` prints `rejected connection from <address>: <reason>`, the reason being the text the client gets, and never the GUID, SteamID or ticket. [Guide](key-codes.md) |
| Finding servers | default | The local list, favourites and `connect` need no setup. The internet list is opt-in: `masterServerName` is empty by default, so no master server is contacted (retail's is gone); when set, the client asks that master and a `dedicated 2` server sends it heartbeats. A list is accepted only from the master that was asked, and since PR 179 the local list shows an unasked reply only when it echoes the random token of the client's last scan and arrives within 10 seconds. Checked asset-free over loopback UDP on hosted CI. In WSL on the PC (local, software rendering) a client got the server from the test master, joined and played, `masterServerName` stayed locked while connected, and the server signed off from the master on quit (server browser run 1); the LAN scan works there too (LAN scan probe 1). The same script (PR 216, merged; menu check run 9 on the PC's WSL, 18 of 18 steps) opened Join Game, listed the server on Local, added it to Favourites, joined it from there and found it again after a restart; its earlier runs found three client faults that PR 216 fixed: the shown window never activated the app without a window manager (so no mouse), Add To Favorites crashed on x86_64 (an unsigned offset read 4 GB past the server status), and favourites were never pinged so a filter hid them. Nobody has opened the menu on a real screen, and no scan of a physical network has run. [Guide](server-browser.md) |
| Singleplayer `KisakCOD-sp`, i386 | `-DKISAK_BUILD_LINUX_SP_GAME=ON` | killhouse and cargoship load, leave the pregame by themselves and play about a minute each, clean quit (laptop). All 21 levels load, run 45 s and quit cleanly; a 4-level mission chain and devsave/loadgame pass (laptop sweep run 1, [levels](linux-sp-levels.md)). Since PR 193 the sweep also checks each level's frame (`--screenshot`) and a death and retry (`--death-retry`); both pass on all 21 levels (laptop). The x86_64 build (since PR 167) also loads all 21 levels and passes the chain and save/load on the laptop, with movies on. `KisakCOD-sp` links the opt-in Vulkan back end and `r_vkStats` (PR 193); on the laptop (x86_64, SP Vulkan run 1, beside other work) killhouse, cargoship, blackout, ac130 and airplane each loaded through `r_backend vulkan`, held 60 s, drew a frame and quit with exit 0, with no unsupported D3D9 call reported. Nobody has looked at those frames, and PR 218 (merged) records the run in the guides. PR 228 (merged) records the PC's sweep of all 21 levels through `r_backend vulkan` on Mesa's llvmpipe in WSL (x86_64, local, not CI): all 21 load, hold 60 s and quit with exit 0, with no unsupported call or `VK_` line; five frames at 60 s are one-colour level openings (the game slows its clock when frames are slow on a software device), and village_defend needs a 420 s hold on Vulkan and then matches DXVK. [Guide](linux-sp-game.md) |
| Vulkan renderer back end | `-DKISAK_BUILD_VULKAN_BACKEND=ON`, then `+set r_backend vulkan` | The game's own D3D9 renderer drawn through this project's Vulkan back end in place of DXVK (Linux) or `d3d9.dll` (Windows), with the original visuals. `KisakCOD-mp` plays mp_crash through it on the laptop (x86_64) and on the PC (the Win32 build from CI), and quits cleanly. The picture matches: the world draw probe's two views agree with DXVK at PSNR 66.7 and 100 (laptop), and on the PC Vulkan and real D3D9 agree at 37 to 44 dB from one indoor view, the same spread as repeats of that comparison. Speed at a fixed mp_crash view on the laptop: since PR 152 (commands recorded on a second thread, a frame handed to the GPU in parts while it is drawn) 140.9 fps against 151.3 through DXVK on mains in power-saver mode (laptop run 9), and 139.5 against 138.2 on battery (laptop run 10, one pass); before it, 116 to 125 against 151 to 159. Those laptop figures are not like for like: the DXVK rows drew 4x anti-aliasing and the Vulkan rows one sample, because the back end had no multisampling until PR 203 (laptop run 13, part 0). At equal samples (laptop run 13, on mains) Vulkan is slower: 135.8 against 176.4 fps at 1 sample (about 23% behind) and 100.7 and 95.7 against 152.5 and 151.1 at 4 (about 35%); four samples cost it about 4 ms of GPU time in the passes a frame, and its 4-sample picture matches DXVK's by mean brightness and a 5x5 grid of cell means. Laptop run 14 (Mesa's `INTEL_MEASURE`) found slow colour and depth clears, about 2 of each a frame and 2.7% of the GPU's time (a first read put it at 6%); PR 220 (merged) folds whole-target clears into the pass's load operations (`KISAK_VK_PASS_CLEARS=0` turns it off), and laptop run 15 saw about 60% of the clears become load operations but no frame rate the laptop could tell from noise. Laptop runs 16 to 20 then went at the shaders: run 16 found the passes' timestamp pairs cost nothing measurable (PR 226 added `KISAK_VK_PASS_TIMESTAMPS=0` to leave them off); run 17 found neither side spills but our shaders two to four times DXVK's length; the fast D3D9 multiply rule (PR 231, the default) gave 22% more frames at 4 samples (run 18); the alpha test and the legacy state the game never changes fixed per pipeline (PR 232) took a further 43.8 fps (run 19); and the depth compare fixed per pipeline (PR 233) another 0.4 ms of GPU time a frame (run 20). At the same 4 samples Vulkan drew run 13's scene 170.5 fps against DXVK's 166.8 in run 19, about 2% ahead, where run 13 had it 35% behind; the pixel shaders' median is 157 instructions against DXVK's 359, and Vulkan compiles 183 pixel shaders against DXVK's 169. PR 234 (merged) copies the frame's timestamps on the GPU so Present never waits on them (a lavapipe-only stall that failed one front check now and then). On the PC the 75 fps cap is gone since PR 173 asks for the IMMEDIATE present mode with vsync off: 154.6 fps against about 203 for real D3D9 at one view, and forcing MAILBOX brings 75 back (PC run 7). Translated shaders and pipelines are kept between runs (PC, Debug build: 15 s to the map warm, 62 s cold). Hosted CI builds it into the i386 Linux client, the macOS client and the Windows game (Win32 and x64), runs the device probe and the adapter and front pixel checks on lavapipe, and runs those two checks on macOS through MoltenVK. The darker models an earlier laptop run showed with `r_altModelLightingUpdate 1` (the default) were the view moving between screenshots, not the back end: measured again over several shots a launch, Vulkan and DXVK on the laptop, and Vulkan and real D3D9 on the PC (PC run 6), draw the same brightness at 0 and at 1, and hosted CI now checks both update paths through the front (PRs 165 and 183). Since PR 203 the back end does less work for a draw like the one before, keeps descriptor sets across frames (`r_vkKeepDrawState`, on by default) and draws multisampled targets; on the PC (run 8, the hosted Debug build) multisampling works with 4 samples and the kept state wrote 0 descriptor sets a frame against 227 without it. Debug leaves the back end unoptimised (Vulkan 171 against real D3D9's 210 fps there, about 22% behind); since PR 208 a manual Windows CI run builds the Release game with it, and in that build (PC run 9) Vulkan made 451 and 462 fps against Direct3D 9's 480 and 482, about 5% behind, and 436 with `r_vkKeepDrawState 0`; in PC run 11 (the Release build of master after PR 232) Vulkan made 477.6, 460.1 and 458.4 fps and Direct3D 9 471.0 and 446.3 by the counters (Vulkan 1.5% ahead) or 485.7 and 459.3 by the game's frame log (1.4% behind), so the shorter shaders do not move the PC, which waits for the CPU. PR 217 (merged) copies only the dirty part of a texture on update: in PC run 10 (Release) texture uploads fell from 9 to 17 GB a launch to about 370 MB and the time spent from 1.8 to 3.3 s to 0.11 to 0.15 s, and Vulkan made 473, 457 and 478 fps against Direct3D 9's 475 and 498, about 3% behind, which is inside the launches' spread. On the laptop the kept draw state is worth about 2 to 3% and the GPU feed 8 to 10% (run 12). Since PR 173 the sun's occlusion queries count samples: in the open every result is 256 of 256 and indoors none, as with DXVK on the laptop (run 11) and real D3D9 on the PC (run 7), and the GPU feed is set from the console (`r_vkGpuFeed`, statistics with `r_vkStats`). PR 252 (merged) makes Vulkan fullscreen behave like Direct3D 9: the gamma ramp is applied (`r_gamma`; a fullscreen pass through a 256x1 ramp texture, 0.060 ms of GPU time a frame), the display mode and refresh rate are switched on the primary display (the monitor choice is primary only) and given back by itself on quit, and the game minimises on losing the foreground and returns at the game's size. PC run 17 (RTX 2080 Super, 2560x1080 at 75 Hz): `r_gamma 0.8` came out darker than 1 (mean 24.3, 26.8, 25.2 against 37.5, 40.6, 38.6), and Direct3D 9's gamma cannot be judged from captures, which read the image before the display's gamma stage. In PC run 18d (a Release build, `r_mode 1280x720`) the display went to 1280x720 at 60 Hz, came back to 2560x1080 at 75 Hz after quit, and three alt-tab trips (two with a helper's restore, one a real keyboard Alt+Tab) returned at (0,0,1280,720) with a 1280x720 swapchain; it took three fixes (runs 18, 18b and 18c) to get there. The first Modern visuals piece (PR 245, merged) is `r_visuals modern` (archived, latched, default `legacy`, which changes nothing): temporal anti-aliasing over the scene at one sample, with a jittered projection, camera reprojection and a variance-clipped history. Laptop run 22 (on PR 245's head 34e6667, the mp_crash scene, a busy desktop so its fps gaps are rough) found it anti-aliased every scene once history existed at about 1.45 to 1.55 ms of GPU time a frame, with fps down from 203.3 (legacy) to 166.7, against DXVK's 4x anti-aliasing at 152.8. Velocity comes from the camera alone, so moving models are softer while they move. Open: the longest frames are pipelines built during play (laptop run 12: 424 ms for 4 of them; PC run 11: 25 to 97 ms against Direct3D 9's 7 to 8); PR 237 (merged) builds them ahead on worker threads from a list of past draws (`pipelines.list`, `KISAK_VK_PIPELINES_AHEAD=0` turns it off), shares one pipeline between identical shader copies, and takes a one-word front lock with kept samplers: in laptop run 21 the workers built the whole list (122 pipelines), the drawing thread's pipeline building fell from 355 and 207 ms on master to 143 and 77 ms, and the load's long frame is gone; in PC runs 14 to 16 it fell from 121 to 131 pipelines built on the drawing thread to 3 to 10 and the first view's 100 ms frame is gone, at 476.2 fps against master's 470.3 (+1.3%) and a GPU frame of 0.66 against 0.80 ms (a 140 to 260 ms frame at about 24 s shows on master too). Shader creation takes 29 to 31 ms over a load against 15, the extra being the jobs' queueing. PR 235 (merged) adds the pixel shaders built for pipelines, with their time, to the front's `device stats` and `slow frame` lines. PC run 12 (the Release build of master after PR 234) found the slow first launch of run 11 was NVIDIA's own shader cache being cold: the first view's 71 pipelines took 1.29 s with every cache cold and 0.25 s once the driver had the shaders; Vulkan made a mean 470.8 fps against Direct3D 9's 478.5, inside the spread of their launches, and PC run 12b found the depth compare read at run time (`KISAK_VK_DEPTH_COMPARE=dynamic`) changes nothing on this CPU-bound PC (461 to 485 fps either way). Off by default. [Guide](vulkan-d3d9-front.md#in-the-game-r_backend-vulkan) |
| Windows x64 | MSVC `-A x64` (`KISAK_WIN64`) | sp, mp and dedi build and link (CI job "MSVC x64 build"); opt-in FFmpeg movies; OpenAL sound, no Steam. On the PC, mp_crash loads to class select, the dedicated server hosts bots, and singleplayer plays: killhouse and cargoship with autosave and checkpoint loads (PR 95), and a 12-level tour loaded, saved and played without a crash or script error (PRs 111 and 114). A tour of all 21 levels (PR 167's build, 2026-11-10) loaded 20; airlift crashes about 4.5 s in, with the older x64 build too, and PR 193 (merged) fixes it: an effect model's physics preset was read at a 32-bit slot. Airlift then held 60 s in three of three launches on the PC, where it had crashed three of three. [x64 track](linux-x64.md#windows-msvc-x64-build) |
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
  (`sv_allowUnverifiedGuids 1`, [key codes](key-codes.md)). Build with `-DKISAK_LINUX_STEAM=ON` and start with `+set cl_steam 1` to authenticate through the
  Steam client running on the machine ([guide](linux-steam.md)). A Windows
  client has joined a Linux server that way, and a Linux Steam client has joined Linux and Windows servers (Steam verdict run 1). Not yet tried: two Steam
  accounts.
- **Singleplayer played by hand past two levels.** All 21 levels load and run
  unattended, and mission changes and save loading work from the console
  ([linux-sp-levels.md](linux-sp-levels.md)). Only killhouse and cargoship
  have been played by hand; night vision, ac130, the Javelin, slow motion
  and the victory screen are untried by hand.
- **A singleplayer save with a moving dynamic entity on x86_64.** PR 193
  stores a loaded entity's physics body as an object id so it survives on
  64-bit builds; no run has loaded such a save yet
  ([guide](linux-x64.md#x86_64-singleplayer-game)).
- **The PS Vita client's speed on the console.** mp_crash loads on the
  console and the default picture looked normal in console run N (the
  doubling of run L is fixed by PR 192). Facing the busy way it runs at 6 to
  7 frames a second, turned away at about 14, limited by the GPU's wait;
  PR 202 cut the back end's own draw time by about 15 ms a frame
  ([guide](vita-client.md)).
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
  are on by default when libcurl 7.85 or newer links (`-DKISAK_LINUX_HTTP_DOWNLOADS=AUTO` since
  PR 247, merged; `ON` and `OFF` force it, macOS stays off until tried, and an old build folder keeps its cached `OFF`
  until it is configured again with `AUTO`). The engine's libcurl downloader
  has been available since PR 170, checked against local HTTP and TLS
  servers on hosted CI, and on the PC's WSL a client fetched a file from a
  game server's redirect with one GET, status 200 and matching sha256 (local
  HTTP download run 1). No client reached the game in that run: an optimized
  server build with `fs_game` set listed the game's own fastfiles as mod
  fastfiles, so no client could join a modded Linux server. PR 205 (merged)
  fixes that and a `+set sv_wwwBaseURL http://...` on a Linux or macOS
  command line that was cut to `http:` (both confirmed by PC run 2). x86_64
  clients then stopped loading the map on an assertion, because the weapon
  file fields were read at 32-bit offsets; PR 209 (merged) fixes it, and with
  it a client reached the game (state 9, in about 100 s) once the server's
  `sv_connectTimeout` was raised above the default 45 s, since a
  software-rendered map load with a mod takes that long (PC run 3, llvmpipe;
  a real GPU is untried). A first start on a fresh client home
  ignores `+connect` because the intro film starts after it, as in the
  original game; `+set com_introPlayed 1` avoids it
  ([guide](linux-mp-client.md#http-downloads)).

## Security

The 2026-11-01 security audit is closed: PR 18 fixed findings 1 to 3 and PR 106
(2026-11-06) fixed findings 4 to 11, widened the download file name check and
built the Linux games and server with `_FORTIFY_SOURCE=2` and the usual
hardening flags (laptop soak run passed). PR 106 also made CI retry slow apt
mirrors instead of timing out. A daily security check now reads what
merged since its last pass. The first (2026-11-10) read everything merged
since that audit; its fixes are PR 191 (merged: sound sizes that wrap in
32-bit math are refused, Windows opens only http and https addresses for a
server, and the Vita reads its text run settings once at start-up so a
server cannot have them run commands; compiled and checked on hosted CI,
not run on Windows, in Vita3K or on a console) and PR 194 (merged: a
server acts on Steam's later verdict for a ticket, so a dropped or refused
player needs a new challenge and ticket; Steam verdict run 1 passed with a
Linux server and with a Windows server on the PC, each with the laptop's
Linux Steam player). PR 225 (merged) stops a game server from setting the
50 settings this port added (`r_backend`, the `r_vk*` and `in_gamepad*`
families, `cl_keyCodeOnly`, `sv_allowUnverifiedGuids` and more) on a joining
client, by systeminfo, codinfo or `setClientDvar`; every retail setting
behaves as in retail, a note prints when systeminfo names a setting the
client does not flag for it, and a new asset-free check fails when a dvar is
registered that is neither a retail name nor on the port's list. Its first
version joined a stock and a modded server in WSL with no refusal line (local);
no mod with scripts of its own has been joined.

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

As of 2026-11-12 04:12 UTC, six. They change by the hour; the
[list on GitHub](https://github.com/koalabuttz/KisakCOD/pulls) is the current
one. A machine run a pull request waits on is named where its author has
said so.

| PR | What | Machine run it waits on |
| --- | --- | --- |
| 214 | PS Vita client (draft): build fragment programs that sample a cube map (`texCUBElod`) without fast maths, for the speckle on the ground under decals | Vita3K run Z11 on its head (2cb543f): warm-up, then the ground views against Windows and run Z8. |
| 249 | CI: the 15 diagnostic artifact upload steps no longer fail a job whose checks passed when GitHub's artifact service is down (it was, from about 20:25 UTC) | None named. |
| 251 | Ultrawide screens use their own shape with `r_aspectRatio auto`, borderless fullscreen (`r_fullscreenMode`, on the server setting list), Display and Controller pages and FPS rows in Options. "Settings pages run 1" (the PC's WSL, Xvfb) passed: the rows, pages and tabs draw and work, borderless covers the screen, Apply keeps `r_mode` and `r_aspectRatio`, a 2560x1080 picture is not stretched. Borderless run 1 on native Windows (the real monitor, a local Win32 Release build of the head) passed B1 to B4: borderless covered the 2560x1080 monitor with no mode change, three Alt+Tab trips came back, Exclusive and Apply switched the mode and back, windowed worked; it also found the Display tab missing after a map (fixed in the PR's last commits, checked offline only) and the menu in exclusive 1280x800 drawn in the ultrawide layout in a capture (the glass was not seen) | None named; the singleplayer menus are not verified. |
| 256 | PS Vita back end: let the CPU fill a frame while the GPU draws the last (discarded buffers and rewritten textures change memory with a kept piece instead of waiting for the whole GPU; the pacing waits for the frame before last; main pool 20 to 26 MB). PC run Z18 in Vita3K: same pictures with and without it. Console run AA (2026-11-12, mp_crash with bots, the overlap on and off inside one start): busy view 153.8 to 154.1 ms a frame (GPU 109) with one wait a frame against 114.4 to 114.6 ms with the overlap, 141.8 against 118.7 ms with seven scenes, 129.4 to 129.6 against 85.3 ms on the console's own settings, and the other way 53.5 against 39.2 ms; every notification came, no lock waited for the whole GPU, the pool peaked at 24.3 of 26 MB, `no-vsync` changed nothing, and the player saw nothing wrong. The frame is now about as long as the GPU's own time | None named. |
| 260 | PS Vita back end (draft): draw the frame in fewer scenes. Console run V counted seven a frame; the forced depth pass now draws into the scene's own targets (with `sc_enable` the engine's way is kept) and the copy of the picture distortion reads is made only in frames that need it; `no-depth-in-scene` and `no-resolve-skip` turn each off. PC runs Z19 and Z19c in Vita3K: the same picture; with the console's settings 3.0 scenes a frame at the busy view and the other way (1.0 fullbright), and with the PC's settings 4.0 against 6.0 and 3.0 against 5.0 | Console run X: the GPU's time with and without the two markers. |
| 261 | Parity batch 2 (O5 to O8): on Linux and macOS a fatal error is shown in an error box, the "computer has changed" and "configuration updated" questions are asked in a Yes/No box (they were always answered No; the first start after PR 246's real GPU name asks once), `Sys_OpenURL` opens a web address in the desktop's browser, and `startSingleplayer` and `startMultiplayer` start `KisakCOD-sp` and `KisakCOD-mp` beside the running program (they started `cod3sp.exe` and Xbox 360 code); `KISAK_NO_DIALOGS=1` turns the boxes off and the check scripts set it; checked on an Xvfb display for the error box, not with game data | A PC (WSL) check of the switch, the question box and a link with game data is written and asked of the PC. |

## Laptop and PC runs still to do

As of 2026-11-12. Each machine does one run at a time. How a run is asked
for, kept short and reported (the `framelog` line, the recipe-driven
frame-rate driver, timed PS Vita starts) is in [runs.md](runs.md), since
PR 168.

- Vulkan back end: `r_visuals modern` on the PC. Laptop runs 12 to 22 and PC
  runs 9 to 18d are done.
- Vulkan probe with the Khronos validation layer (synchronisation checks) on a
  real GPU; the laptop and PC runs so far had no layer installed. The
  standalone Legacy adapter and D3D9 front checks have no recorded PC run
  either (the game itself draws through both there).
- PS Vita client: console run P (where the graphics chip's time goes; the
  steps are written); Vita3K run Z11 on PR 214's head; console run X of PR 260's fewer scenes; the keyboard on the console; a listen of the
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
  a 30-minute session on one map. An HTTP download to a modded server on a real GPU (runs 1 to 3 on the PC's WSL are done).
  The setup tool on a real GPU and monitor (its first run, on the PC's WSL,
  passed with software rendering), and a real controller in the game.
- Finding servers (PR 163, merged): the Join Game menu on a real screen, and
  a LAN scan across two machines ([guide](server-browser.md#what-is-checked));
  server browser run 1 and LAN scan probe 1 in WSL are done.
- Key code menu (PR 159, merged): a key typed into the menu on a real screen
  ([guide](key-codes.md#the-key-code-menu)); the scripted check passed in WSL.
- CPU measuring: `r_gpuSyncSleepUnlocked` (PR 197, merged, off by
  default) on a GPU. Laptop perf run 5 is done ([guide](perf-ab.md)).

## Roadmap

The PS Vita client is the stated priority; the other tracks run beside it, in
no fixed order.

1. **Land the open work above**.
2. **PS Vita client** (the priority since 2026-11-09): the menu and
   mp_crash run on the console (console run L), with the dynamic GPU
   buffers cut to fit (PR 166) and shader programs made in the background
   so the menu does not freeze (PR 164). Next is speed (PR 202 cut the ground view's frame time from 67 to 30 ms
   in Vita3K and the back end's draw time on the console by about 15 ms;
   the console's GPU wait still dominates, console run P times it, and in a
   Vita3K diagnostic run one program pair took about half the GPU time of
   the turned-away view in two draws), the decal speckle (PR 214), the other maps (all 21 start in Vita3K; mp_carentan runs out
   of GPU memory), text typed on the console's own keyboard (PR 162 is
   merged and checked in the emulator), and joining a server
   ([guide](vita-client.md#stages)).
3. **Renderer**: the Vulkan back end with the original visuals draws the game
   on the laptop and the PC (PRs 124 to 152), on the PC's Release build
   level with real Direct3D 9 within the spread of their launches (runs 9 to
   11), on the laptop about 2% ahead of DXVK at equal samples (run 19; it was
   35% behind in run 13) after shorter shaders (PRs 231 to 233). The PC's 75
   fps cap and the occlusion queries are fixed (PR 173); multisampling, the
   kept draw state (PR 203), dirty-only texture uploads (PR 217), pass
   clears (PR 220) and fullscreen gamma, display mode and alt-tab (PR 252) are merged. Next is the pipelines built during play (the
   long frames on both machines; PR 237 built them ahead), the render
   thread's time per draw, the open differences in the table above, and the
   checks on more GPUs. The Modern path has begun with temporal
   anti-aliasing (PR 245; plan in the project files,
   `renderer/modern-renderer-plan.md`). The target is
   Modern at 60 fps on the laptop's Intel U300.
4. **Linux client polish**: hand play on the other maps (all 21 pass the
   unattended sweep), a long session, an HTTP download on a real GPU (PRs 205
   and 209 got one to the game under software rendering), then voice chat and the remote
   script debugger
   ([list](linux-mp-client.md#what-it-does-not-have-yet)).
5. **Linux singleplayer beyond two levels**: hand play of the swept levels
   (all 21 pass the unattended sweep on i386 and x86_64), a save with a
   moving dynamic entity on x86_64, and the frames of the five levels
   that loaded through the Vulkan back end and the other 16 levels.
6. **Performance on the CPU**: the laptop A/B runs and the function-level
   profile are done (PRs 97 and 112). Their reading is that the laptop is
   limited by its GPU at both sizes measured, so CPU work there is about
   power and long frames. Measuring inside one launch is merged (`perf_ab`,
   PR 160, [guide](perf-ab.md)), because the laptop's speed drifts more
   between launches than most changes are worth; its first laptop run (perf
   run 5, battery, mp_crash with 12 bots) made sleeping in the waits for
   the GPU the Linux default (`r_gpuSyncSleep 100`, PR 197): 2.8 % more
   frames and 2.75 ms less CPU a frame at 1280x800, 12.6 % and 8.25 ms at
   1920x1200. One switch it brought, `xanim_blendOnce` (off by default),
   calculated 131072 animation blends both ways on the PC with none
   different.
7. **Remote play**: Steam login is opt-in (`KISAK_LINUX_STEAM`, PR 85); a
   Windows client joined a Linux server logged on as an anonymous game server
   (PR 133), and a client without Steam joins servers that accept key code
   GUIDs (PR 134). A Linux Steam client has joined anonymous-login servers
   on Linux and on Windows (Steam Windows run 3, Steam verdict run 1, PR 194), and
   a Windows key code client has joined Windows and Linux servers (Windows
   key code run 1); a Steam player and a key code player shared one server
   (mixed session run 1), and a Linux listen server let two remote key code players in (listen server run 1). Next: two Steam accounts, and the internet list seen working in the
   Join Game menu (PR 163 made the master a setting).
   Admission is never weakened to fake it.
8. **Other platforms**: an aarch64 DXVK build and a first Arm run with game
   data, Windows ARM64 with game data, the macOS client with game data
   through `r_backend vulkan` (first run on an Intel MacBook Pro; a release
   bundle also needs MoltenVK and the Vulkan loader), and bots on the PS Vita
   dedicated server ([guide](vita.md)).
9. **Security**: the daily check's first fixes are merged (PRs 191 and 194).

## Checks before a push

```sh
python3 scripts/tests/run_pre_push_checks.py
```

Since PR 243 that one command runs, four at a time, the Linux bootstrap test,
the runner self-tests, the workflow contract test and the CI script tests,
plus `run_linux_checks.py` when `build-linux/` and `bin/KisakCOD-dedi` exist
(about two minutes; `--list` shows the set). Run on its own, the main
asset-free check is still:

```sh
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
