# PS Vita multiplayer client

`-DKISAK_BUILD_VITA_MP_CLIENT=ON` (with `KISAK_PLATFORM=vita`) builds the
multiplayer client, `KisakCOD-mp`, for a hacked PS Vita and packs it as
`KisakCOD-mp.vpk` (title ID `KSKD00002`). It is opt-in and off by default, and
it follows the [Vita dedicated server](vita.md), which shares its toolchain,
platform layer and memory rules. Started 2026-11-09.

**State: stages 1 to 3 run on the console. On 2026-11-10 the main menu
showed on David's Vita and worked, with touch, buttons and music (run K),
and later that day mp_crash loaded there and a player walked around in it
(run L, "Memory" below): nothing was refused GPU memory, and with the
picture right the console drew 5 to 8 frames a second. The picture was
right only with depth of field and soft particles off: with them on the
console showed two and three images at once, from a depth clear it
refuses. The fix for that is built and has not run on a console yet
("Clears" under "Graphics").**
- The whole client compiles, links and packs into a `.vpk`: client, cgame, UI, sound, renderer and its own listen server.
- Stage 1 had no graphics back end, so the client stopped at the renderer through `Sys_Error`. Stage 2 replaces that: the renderer gets the D3D9 front on the GXM back end ("Graphics" below).
- Stage 3 (controls and sound) runs on the console: on run 1 (2026-11-09) the menu music and the menu's sounds were heard, and every button arrived as its key. The screen's touch and the layout in a game are not confirmed yet: see "Controls" and "Sound" below.

**Runs on David's Vita (2026-11-09):**
- Run 1 reached the renderer, but its log was empty. A game client's `Sys_Print` was silent: on Linux `Com_PrintMessage` copies lines to stderr, which the Vita lacks. It then aborted on an assert, because `R_SetWndParms` read an empty display-mode list after the "no device" answer. Both are fixed.
- Run 2 stopped where expected: the banner at 1.6 s, `FS_Startup` at 2.0 s, the network up at 8.8 s, then `R_Init` and the fatal error at 9.0 s. No zones load before the renderer starts.
- At the error: zone arena 128 MB free, heap 5.8 of 64 MB used, engine reservations 154 MB, and **61 MB of system memory free** (220 MB free at start-up).
- Run 2 also showed every `.iwd` opened twice: newlib's `getcwd` spells the folder `ux0:/data/KisakCOD`. `Sys_Cwd` now drops that slash.

## Building

As for the server (see [vita.md](vita.md)), plus two header directories:
- **DXVK Native's headers** supply the D3D9 declarations: the types and interface layouts only. No DXVK code is built or linked. The pinned 2.7.1 headers come from `scripts/tests/run_native_d3d9_boundary_checks.py --stage-dxvk-headers`, as in the Linux client jobs.
- **OpenAL's headers** (`AL/al.h`, `AL/alc.h`, `AL/efx.h`) come from libopenal-dev or the OpenAL Soft source, for the declarations only. No OpenAL library is linked ("Sound" below).

```sh
export VITASDK="$(scripts/ci/install-vitasdk.sh "$HOME/vitasdk-pinned")"
cmake -S . -B build-vita-mp -G Ninja -DKISAK_PLATFORM=vita -DCMAKE_BUILD_TYPE=RelWithDebInfo \
  -DCMAKE_RUNTIME_OUTPUT_DIRECTORY="$PWD/out/vita-mp" \
  -DKISAK_BUILD_VITA_MP_CLIENT=ON \
  -DKISAK_DXVK_NATIVE_INCLUDE_DIR=<staged dxvk headers>/dxvk \
  -DKISAK_OPENAL_INCLUDE_DIR=<directory holding AL/al.h>
cmake --build build-vita-mp --target KisakCOD-mp-vpk
# -> out/vita-mp/KisakCOD-mp (ELF with symbols) and out/vita-mp/KisakCOD-mp.vpk
```

Hosted CI: the **PS Vita multiplayer client** job of the PS Vita workflow
(`.github/workflows/build-kisakcod-vita.yaml`) builds Debug
and RelWithDebInfo. It runs the server's float checks (no fused multiply-add, no
NEON float) and link checks (weak pthread references, the main stack size), and
uploads the RelWithDebInfo `.vpk`. It also runs the controls' and the sound's
host tests (`scripts/tests/test_vita_input.py`, `test_vita_sound.py`), the run
plan's, the GPU heap's, the frame time figures' and the shader translator's
(`test_vita_run_plan.py`, `test_vita_gxm_heap.py`, `test_vita_frame_time.py`,
`test_vita_cg_translate.py`), and the static model cache's size arithmetic
(`test_static_model_cache_sizes.py`).

## Running

1. Install `KisakCOD-mp.vpk` with VitaShell, beside the server. The game data
   is the server's, in `ux0:data/KisakCOD`.
2. Optional: write a command line to `ux0:data/KisakCOD/cmdline-mp.txt`. The
   client has its own command line and log, so the server's `cmdline.txt` and
   `console.log` stay as they are. With no file, it starts with no arguments,
   as the PC game does.
3. Start the bubble. The log is `ux0:data/KisakCOD/console-mp.log`. Configs
   and the player profile go to the same folder (`fs_homepath`).
4. Expected now (stage 2, step 2d): start-up, the file system, then
   `Graphics back end: GXM`, the GPU memory lines (`gxm legacy:`, `gxm:`),
   the front-end zones and the menu loop, drawn. Every shader the game
   creates is translated to Cg and its program compiled on a thread of its
   own (`gxm legacy: shaders so far: ...` and `programs in the background
   so far: ...` every 100 shaders, `in all` at the end), and the compiled
   programs are kept in `ux0:data/KisakCOD/gxp`. On a first start the menu
   fills in piece by piece while its programs compile; a frame that still
   takes over 250 ms is logged as `long frame N: ...` with what the loading
   thread did meanwhile. `gxm legacy: draws: ...` lines at the
   first frame, after ten seconds and every minute count what was drawn and
   what was skipped, and why. The client needs
   `ur0:data/libshacccg.suprx`, as the GXM probe does; without it the log
   says so and nothing is drawn.

START + SELECT quits. For a run nobody watches, `+set vita_quitAfter 90` in
the command line file quits by itself 90 seconds after start-up (the
dedicated server takes it too), `+set vita_quitAfterMenu 15` 15 seconds after
the main menu first showed, and `+set vita_quitAfterMap 30` 30 seconds after
the map was first in play; each prints every figure first. `vita_atMap` sends
timed console commands, for example to pick a team and a class, and
`vita_assertExit 1` keeps an emulator from hanging on a failed assertion. See
[Runs nobody watches](vita.md#runs-nobody-watches).

To see what a translated shader looks like, put an empty file named
`dump-cg` in `ux0:data/KisakCOD/gxp`: every program asked for then also
leaves its Cg source there as `<same 16 hex digits>.cg`. Those sources come
from the game's shaders, so keep them on that machine like the game's files.
A file named `run-tests` in the same folder turns on the draw tests of
`gxm_legacy_draw.cpp` (`kGuessEdits`): for the first minute and a half the
screen shows test pictures, mostly plain green, and the log says what each
left in the back buffer. A file named `read-back` there has the client log
only what frames 300 and 700 left in the back buffer (`diag frame 300 ... back
buffer: N of 522240 pixels not black`), which on a console says whether a
picture was drawn. Without either file the client logs frame 300's first
draws and reads nothing back. Leave both files out in Vita3K unless a run is
about them: once the game has read a display buffer, the emulator draws every
later frame into that buffer slowly. Up to PC run H1 the two reads were made
in every run, and the menu there fell from 43 frames a second to 12 after
frame 300 and to 6 after frame 700 (a frame of 250 ms in every three), while
the read itself counted no lit pixel. Without the reads (PC run H2,
2026-11-10) the same menu start drew 5135 frames in 120 s where it had drawn
1174, and a map start with a filled cache drew 4473 frames in 180 s, 25 a
second by the game's counter, where H1's had drawn 868 and shown 2. Frame
times from Vita3K runs before that are therefore the emulator's, not the
game's.

Two switches are for measuring in an emulator what the game would take if it
had the room; a console has no memory for either:
- a file named `big-gpu-pool` in the same folder adds 96 MB to the GPU's main
  pool, so a map's buffers and textures can all be made and the
  `gpu memory by use` line says what they come to;
- `-DKISAK_VITA_HEAP_MB=128` at configure time builds a client with a 128 MB
  heap instead of 64.

## What it builds

`scripts/platform/vita/mp_client.cmake` takes the same engine sources as the
Linux client (`scripts/platform/linux/mp_client.cmake`):
- the census-covered groups;
- minus what the client census blocks: the Windows platform layer, DirectSound voice and curl downloads;
- r_cinematic.cpp, built without CINEMA (no Bink);
- the renderer units on the native D3D9 proof list, compiled through the boundary.

Around them:

| Piece | File | Notes |
|---|---|---|
| Platform layer | `src/linux/*.cpp` + `src/vita/*.cpp` | The server's, minus its stand-ins for client systems |
| Win32 window calls | `src/vita/client/vita_win32_window.cpp` | One window: the 960 x 544 screen. One monitor |
| Window procedure, input | `src/vita/client/vita_client_input.cpp`, `input/vita_input_map.cpp` | Sets `g_wv.hWnd`; the pad and the touch panels become engine keys and mouse motion ("Controls") |
| On-screen keyboard | `src/vita/client/vita_keyboard.cpp`, `input/vita_text_entry.cpp` | The system's keyboard for menu text fields, chat and console commands ("Typing") |
| Graphics device | `src/vita/client/vita_d3d9.cpp` | The D3D9 front's host: one 960 x 544 mode, one monitor, the log |
| D3D9 front | `src/gfx_vk/d3d9` | The Vulkan work's IDirect3D9, unchanged, over the seam in `vk_d3d9_backend.h` |
| GXM back end | `src/gfx_gxm` | The Legacy API on sceGxm (`legacy/gxm_legacy.cpp`), the screen, GPU memory |
| Sound | `src/vita/client/sound/vita_al.cpp`, `vita_al_output.cpp` | The OpenAL calls the engine makes, on a small mixer, played through SceAudioOut ("Sound") |
| Linux client pieces | `msvc_rand.cpp`, `mp_client_unavailable.cpp`, `texture_memory.cpp`, the Material_Sort span | As on Linux |

`deps/binklib/radbase.h` gains a `__vita__` branch (the Linux one, with the
Vita's CPU), because the renderer's headers declare the Bink API.

**Run K on David's Vita (2026-11-10, commit d4a03fa):**
- The menu showed and worked: frame 300 left 480359 of 522240 pixels not black, the first frame came 44.5 s after the start, touch and buttons reached the menu and the music played.
- The menu froze several times, once for 23.6 s, with the music paused. Programs were compiling while the menu was up: 139 in 94 s, about 0.7 s each, and 672 more were read from the card in 16 s. The game was still loading `common_mp` behind the menu, and its loading thread compiled each shader inside the renderer's lock, so the frame being drawn waited ("Programs in the background" below, which takes that work off both threads; run in Vita3K, not yet on a console).
- Starting a listen server on mp_crash stopped the game 58 ms into the map's fastfile: `out of GPU memory` for a 13 MB vertex buffer, then the renderer's fatal error ("Memory" below).

## Memory

The client links the whole server for its listen server. At the server's 24-player cap, the server's snapshot buffers (`svs`) take 67 MB. The client build caps its own server at **8 players** (`SV_MAX_CLIENTS`, `bg_local.h`), which brings `svs` to 25 MB.

Static data of the RelWithDebInfo build (2026-11-09):

| | Client | Server (24 players) |
|---|---:|---:|
| `.text` | 3.0 MB | |
| `.bss` | 66.2 MB | 88.7 MB |

Other large static arrays in the client:
- `cls` 2.9 MB, `odeGlob` 2.8 MB, `gfxBuf` 2.3 MB (1.5 MB since the smaller skinned caches below), `s_backEndData` 2.2 MB, `tess` 2.2 MB;
- `clients` 1.7 MB, `g_streamState` 1.7 MB.

The heap and the engine reservations are the server's for now: a 64 MB heap, the 128 MB zone arena and the hunks. Textures go to the GPU's own memory (CDRAM, about 112 MB), not main memory.

What the menu takes (run K on the console, and the same build in Vita3K, 2026-11-10):
- **Heap:** 55 of 64 MB once `common_mp` is loaded. About 40 MB of that is the menu's 702 loaded sounds, held as 16-bit PCM as the fastfiles hold them. At the failed map load the console's heap stood at 60.7 MB. That build kept every sound that way; with `snd_adpcm 1`, the default since, the same menu holds 23.8 MB in Vita3K (run S, under Sound below), which is not measured on a console yet.
- **System memory:** 15 MB free on the console beyond the heap and the reservations, so the heap cannot simply grow. Vita3K reports far more free system memory than a console has (107 MB at the same point); never size memory from its number.
- **Zone arena:** 72 MB of 128 free in the menu, 46 MB at the failed map load.
- **GPU:** 95 of 104 MB of CDRAM and 18 to 19 of the 20 MB main pool. By use, from the `gpu memory by use` log line:

| Use | MB | Note |
|---|---:|---|
| Compressed textures | 33.7 | 899 textures at picmip 2; only five are 1 MB or more |
| Dynamic vertices | 19.5 | the static model cache (8), the two skinned caches (4.5 each), smaller ones |
| Depth | 14.1 | the sun's shadow map (8), the spot shadow map (4), the scene's (2) |
| Render targets | 12.6 | the shadow maps' colour targets (4 and 2), three screen-sized ones (2 each) |
| Dynamic indices | 10.0 | five of 2 MB |
| Static vertices | 9.2 | `common_mp`'s vertex buffer |
| The display's buffers | 8.0 | |
| Static indices, volumes, other textures | 2.7 | |

The main pool is too small for the dynamic buffers, so 14.5 MB of them spill into CDRAM. mp_crash's first vertex buffer alone is 13.4 MB, and neither pool had that much left. Making a map fit is the work in progress. The table above is the state before it; so far:
- **Targets take memory when first used.** The renderer makes every render target and depth texture it might use at start-up. On the Vita each one gets its GPU memory when a scene first draws to it, or when it is first locked or copied from (`EnsureTargetMemory`). Until then a draw that samples it reads black. The log names each one as it takes its memory (`gpu memory piece`).
- **A reserve for the targets a game frame needs.** In a map, first use comes after the map's textures and vertices have filled the pools: in runs M and U the two screen-sized targets were refused with 4.5 MB of CDRAM free in pieces, none of them 2 MB long. The back end sets 4.5 MB of CDRAM aside at start-up for colour targets alone (`target reserve` in the log): room for two screen-sized targets and the four quarter-size ones of glow, depth of field and blur. Colour targets take from it first and from the pools after that. A file named `no-target-reserve` in the program cache folder leaves the reserve out, for checking in an emulator what a full pool does.
- **A target without memory.** A refused target tries again each second; the log names the first refusal and the moment it gets its memory. Until then draws into it are skipped and counted (`its colour target has no GPU memory`), clears of the depth bound with it still happen, and a draw or copy that samples it is skipped too (`it samples a render target that has no GPU memory or was not filled`), which passes the mark on to its own target. So a missing target costs the effects that need it (film colour, glow, depth of field, blur, soft particles, distortion) and not the frame. In Vita3K runs M and U mp_crash loaded and a player spawned, but the world was black behind a correct HUD: the two screen-sized colour targets of a game frame were refused. One is the copy of the scene the post effects read, and the film colour pass painted the frame from it. The other is the depth copy (`r_floatz`, R32F) that soft particles and depth of field read; the renderer clears the frame's depth while that target is bound, and nothing cleared it.
- **No shadow maps.** `sm_enable` is 0 and read-only in the Vita build. The back end cannot sample depth textures yet, so the shadow maps would be drawn and never read. With the change above, their 18 MB of targets are never taken.
- **Smaller dynamic buffers** (run V, before the static model cache came back). The engine's dynamic buffers are sized for the PC game: 25.5 MB before any copy. The Vita build (`KISAK_GFX_GXM`, the sizes in `r_gfx.h` and `r_staticmodelcache_sizes.h`) makes them 14.5 MB:

| Buffer | Retail | Vita | When it is full or absent |
|---|---:|---:|---|
| Static model cache | 8 MB, and 2 MB of indices in main memory | 4 MB, and 1 MB of indices in main memory | a model that finds it full of the last four frames' models is drawn by itself that frame, from the map's own buffers |
| Skinned caches, two taking turns | 4.5 MB each | 3 MB each | a model that finds no room is not drawn that frame |
| Pretess index buffers, two taking turns | 2 MB each | 1 MB each | world surfaces that find no room are drawn unbatched |
| Dynamic index buffer | 2 MB | 2 MB | |
| Dynamic vertex buffer | 1 MB | 1 MB | |
| Code and mark meshes, two of each | 1.5 MB | 1.5 MB | |

- **The sizes are estimates, and the log reports them.** Once a minute the log says how full each got (`r_dynamicBufferReport`, in seconds; 60 on the Vita, 0 and so silent elsewhere): `dynamic buffers, the most one frame asked for in N frames: skinned cache X of 3072 KB (N models left out), pretess X of 524288 indices (N batches left unbatched), dynamic indices X of 1048576, dynamic vertices X of 1024 KB; static model cache X of 2048 KB at most (N times a model found it full and was drawn by itself)`. A skinned cache figure above its size is what the frame wanted. In run V (one player on mp_crash) the most a frame asked for was 100 KB of skinned cache, 178056 pretess indices, 5904 dynamic indices and 107 KB of dynamic vertices, and nothing was left out or left unbatched. A full game's players have not been measured.
- **The static model cache is back, at half its size.** The cache holds static models' vertices already moved to their place in the world, so that all of one model in view go out in one draw. The first Vita sizes left it out to save its 8 MB, and in Vita3K mp_crash then took about 3600 draws a frame where it had taken 1600, at 14 frames a second against 25 (PC runs X and H2, 2026-11-10; the two builds differ in more than the cache, so this is a reading and not a measurement). The Vita build now makes it with 64 trees a section instead of 128: 4 MB of vertices, asked for as a write-only buffer so that it goes to CDRAM and not to the main pool, and 1 MB of indices in main memory. It is never locked with DISCARD, so it is never renamed. The size is what a console can spare: mp_crash left 6.5 of its 104 MB of CDRAM free with no cache (run L). PC run AB measured three sizes in Vita3K on mp_crash's ground view (2026-11-10, one build, starts alone): no cache 67.0 ms a frame and 3321 draws, 2 MB 58.4 ms and 2697 draws (full in every frame, 328 models a frame drawn by themselves), 4 MB 46.5 ms and 2059 draws (7 a frame); the pictures were the same and Vita3K's CDRAM peak rose from 97.5 to 99.5 of 108 MB. What a view asks for is more: the Windows build's cache of 8 MB held 2.2 MB at mp_crash's team menu, which looks over the map, and 6.3 MB after a walk (`r_smc_stats`; it frees nothing before it is full). A full cache gives up the trees no frame has used for four frames, and a model that still finds no room is drawn by itself that frame, so too small a cache costs draws and never a model; the log's `dynamic buffers` line counts those. `r_smc_enable 0` draws every static model by itself again, for comparing. `KISAK_VITA_SMC_SECTION_TREE_BITS` (CMake, 1 to 7; 5 is 2 MB) builds another size. The arithmetic is checked at every size by `scripts/tests/test_static_model_cache_sizes.py`.
- **A renamed buffer's copies stay in its own pool.** The engine locks most dynamic buffers with DISCARD every frame. The back end then gives the buffer fresh memory (a rename) and frees the old once the frames that might read it are done, up to three frames later, so a buffer can hold four times its size. Those copies used to spill from the main pool into CDRAM, where they took the room the scene's render targets needed. Now a rename takes memory from the buffer's own pool only (main, for dynamic buffers). With no room there, the lock ends the scene, waits for the GPU and writes in place, which costs time and no memory. One wait serves every buffer drawn before it: after it the GPU reads none of them, so until a draw reads one again its locks write in place without a rename or another wait. The draw summary counts all three: `buffer renames: N took fresh memory, N waited for the GPU instead (their pool was full) and N needed neither (a wait since their last draw); old copies the GPU might still read took N KB at most`. A refused rename no longer writes two lines to the log each time. In run V's 1356 frames 248 renames took fresh memory, 953 waited and 2013 needed neither, and old copies took 3072 KB at most. What the waits cost in frame time on a console is not known.

**In a game, on the console.** Run L (2026-11-10, David's Vita, a build of master with the Vita changes up to the stereo frame count fix): mp_crash loaded for the first time on a console, in three starts.
- **The load:** the map was up 106 and 122 s after a start from the command line (starts 1 and 3), and 61 s after the server was started from the menu (start 2). Nothing was refused GPU memory and nothing failed; every start quit cleanly.
- **Memory:** CDRAM 95.5 MB as the map came up and 97.5 of 104 at most, so 6.5 MB spare on this map. Main pool 19.8 of 20. Heap 35 to 36.5 of 64 MB, system memory 16 MB free. By use the figures are close to run V's (the table below): dynamic indices 6.0 MB where run V had 4.0, and the render targets 4.0 to 6.5 MB.
- **The picture was wrong with the default settings** (starts 1 and 2). David: "I was seeing double/triple. the view changed as I moved the camera but not as you would expect", and it would "randomly snap into looking more normal" while moving and looking around. The log had one line of its kind, 35 ms after the map was up: `draw skipped: the fragment program could not be made: sceGxmShaderPatcherCreateFragmentProgram 0x805b0009`, counted once. Read from the code, that program is the clear of depth and stencil before the float-Z pass, which a console refuses and Vita3K makes; it was left out every frame after that one count ("Clears" under "Graphics" has the fix).
- **With no float-Z pass the picture is right** (start 3: `+set r_dof_enable 0 +set r_zFeather 0`, so no frame needs one). David: "image was fixed". The 0x805b0009 line is gone from the log.
- **Speed:** 5 to 8 frames a second in start 3, walking around with the picture right (163 to 243 frames each 30 s), the system's load line at 100% busy. Starts 1 and 2 drew up to 20 a second with the faulty picture. Depth left standing from earlier frames rejects pixels before they are shaded, which probably spared the GPU most of its work, so those figures are no measure of the game's speed. Where a frame's time goes on the console is not known yet: that build had no `frame time` line.
- **Programs:** the first start compiled 127 programs on the console in 101 s, beside the game, and left 329661 draws out of 1300 frames while it did; the second compiled 104 and left 3739 draws out of 101 frames; the third compiled 54 and left 27275 out of 104 frames, with none waiting at the end. The program cache on the card grew from 803 files to 1088. So the first minutes on a map get better with each start.
- **The start-up benchmark** measured 2.35 GHz on the console (18.36 in Vita3K on the PC) and, of the settings compared, chose the same as there but for `fx_marks 0` and `ragdoll_enable 0`.

**In a game, in Vita3K** on the Windows PC (builds made on that PC, 2026-11-10; not a console):
- **Run M** (before `snd_adpcm`): on the 64 MB heap the mp_crash fastfile loads and then the heap runs out. On a 128 MB heap the map loads and a player spawns, with 71 MB of heap in use.
- **Run U** (with `snd_adpcm`, targets taking memory at first use and no shadow maps; before the target reserve and the smaller buffers): the map loads on the 64 MB heap and a player spawns. Heap 31.8 MB after the load and 32.9 MB in the game; zone arena 46 MB free. Menus and the HUD are drawn. The world is black.
- **Why black in run U:** both pools were full, CDRAM at 103.5 of the 108 MB Vita3K offers (a console has 104) and main at 19.3 of 20. The scene's two screen-sized render targets, 2040 KB each, found no room 0.7 s after the game's own start-up (`CL_InitCGame`), and every draw to them was skipped: 571753 draws, against 997449 drawn. 2308 allocations were refused in seven minutes. All but those two were fresh copies for renamed dynamic buffers.
- **Run BW** (run U's build with the target reserve and the rule for a target without memory; still before the smaller buffers): the world is drawn. Ground, buildings and the held weapon are in colour under the HUD at five looks while turning and walking. Both screen-sized targets take their memory from the reserve, no draw is skipped, and 842866 draws and 1172 copies are made in five minutes. The pools are as full as in run U: 1667 allocations refused, 1666 of them fresh copies for renamed dynamic buffers and one a cube map of 4104 KB, which gets a grey stand-in. About 2 frames a second, as in run U. With the marker `no-target-reserve` the same build draws the world without its film colour, the two targets refused as in run U.
- **Run V** (run BW's build with the smaller dynamic buffers and renames kept to the buffer's own pool): the world is drawn as in run BW, with the weapon, trees, cars and the HUD, over six minutes of walking and looking around. No allocation is refused at all, where run BW had 1667. The 4 MB cube map gets its memory. Every dynamic buffer is in the main pool. CDRAM peaks at 95.5 of 108 MB and the main pool at 19.8 of 20. 1395563 draws, none skipped. Heap 32.9 MB in the game.
- **The sky in run V** is a flat, very bright white. In run BW it was the grey stand-in of the cube map that run V finds room for, so no run has shown a sky picture yet. Whether that is a fault of this build is not known: nobody has put the same view beside the Windows build. The same goes for the strong yellow of sunlit ground in runs BW and V.
- **Frame rate.** The game's counter shows 2 frames a second in runs U, BW and V. Do not read the game's speed from it, even for an emulator: those builds read the back buffer at frames 300 and 700 for their diagnostics, and after such a read Vita3K draws every later frame slowly (in run H1 the menu drew about 43 frames a second up to frame 300, about 12 up to frame 700 and about 6 after it). A build that leaves the reads out (run H2, 2026-11-10) drew the same map at 25 frames a second by the game's counter. That build did not have run V's buffer changes, so what the waits for the GPU cost a frame there is not measured yet.

| Use in the game, CDRAM + main, MB | Run U | Run V | Note |
|---|---:|---:|---|
| Compressed textures | 34.9 | 34.9 | 1569 pieces |
| Static vertices | 29.0 | 29.0 | `common_mp` 9.2, mp_crash 12.8 and 7.0 |
| Dynamic vertices | 4.5 + 15.5 | 0 + 8.5 | in run U one skinned cache did not fit the main pool |
| Dynamic indices | 8.0 | 0 + 4.0 | run U: three buffers of 2 MB and one copy; run V: one of 2 MB and two of 1 MB |
| Other textures | 6.5 | 6.5 | |
| Cubes | 6.5 | 6.5 | 21; one of 4 MB |
| Static indices | 3.1 | 3.1 | |
| Depth | 2.0 | 2.0 | |
| Volumes | 1.0 | 1.0 | |
| Render targets | 0.4 | 4.4 | run U: three small ones, the screen-sized ones got none; run V: those and the two screen-sized ones |
| Both pools, in use | 103.5 + 19.3 | 95.5 + 16.8 | of 108 + 20 in Vita3K; a console has 104 of CDRAM |

The display's buffers (8 MB of CDRAM) and the context's ring buffers (in the main pool) are not in the by-use line.

## Controls (stage 3)

The multiplayer engine takes keys and a mouse. It has no gamepad path. So the
Vita's controls become engine keys and mouse motion, and the game's own
`bind`, controls menu, sensitivity and pitch inversion work on them. No engine
source changes.

- `src/vita/client/input/vita_input_map.cpp` is the mapping. It makes no
  engine call and no Vita call, so `scripts/tests/test_vita_input.py` builds
  it on the host with sanitizers and drives it with made-up readings.
- `src/vita/client/vita_client_input.cpp` reads the pad
  (`sceCtrlPeekBufferPositive`) and both touch panels (`sceTouchPeek`) on the
  main thread whenever the engine asks for events, at most every 4 ms. Keys
  go into the event queue as `SE_KEY`, as on every platform. `IN_Frame` hands
  the menu cursor or the look motion to `CL_MouseEvent`. That call's answer
  says who has the mouse, a menu or the game, and that decides how the next
  reading is mapped.
- `src/vita/client/input/vita_text_entry.cpp` decides when the on-screen
  keyboard opens and what the game gets when it closes; the same host test
  runs it. `src/vita/client/vita_keyboard.cpp` is the system's keyboard
  itself ("Typing" below).

**On the console.** Run 1 (2026-11-09, in the menus, with nothing drawn yet
and `vita_inputDebug 1`): every button arrived as the key in the tables
below, the right stick as the wheel, and a start-up without a profile got the
default layout and the profile name. Run K (2026-11-10, with the menu on the
screen): the screen works as the menu's pointer. The menu was worked by touch
up to starting a server, and the log has nine touches, each a `MOUSE1` press
and release within about 20 ms. Touches in the top-left, top-right and
bottom-left corners of the panel landed in the same corners of the engine's
960 by 544 screen. Not confirmed yet: the
screen's four areas in a game, which side of the rear pad is which, the
look speed and the layout in a game, which need a game on the screen, and
the on-screen keyboard.

### In the game

Every control is a key of its own: AUX1 to AUX16, the keypad arrows and the
keypad's 5, which nothing else on a Vita sends. The default layout is the
console game's where the Vita has the button.

| Control | Engine key | Default binding | Does |
|---|---|---|---|
| Left stick | `AUX9` up, `AUX10` down, `AUX11` left, `AUX12` right | `+forward`, `+back`, `+moveleft`, `+moveright` | Move |
| Right stick | mouse motion | | Look |
| R button | `AUX6` | `+attack` | Fire |
| L button | `AUX5` | `+speed_throw` | Aim down the sight |
| Cross | `AUX1` | `+gostand` | Jump, stand up |
| Circle | `AUX2` | `+stance` | Crouch; hold for prone |
| Square | `AUX3` | `+usereload` | Use, reload |
| Triangle | `AUX4` | `weapnext` | Next weapon |
| D-pad up, down, left, right | `KP_UPARROW`, `KP_DOWNARROW`, `KP_LEFTARROW`, `KP_RIGHTARROW` | `+actionslot 1` to `4` | The console game's d-pad |
| Select | `AUX7` | `+scores` | Scoreboard |
| Start | `ESCAPE` | fixed | Menu |
| Rear touch pad, a tap on the left or right half | `AUX8`, `AUX16` | `+breath_sprint` | Sprint (the left stick's click on a console); while aiming, hold your breath |
| Screen, left third | `AUX13` | `+smoke` | Special grenade |
| Screen, right third, top half | `AUX14` | `+frag` | Frag grenade |
| Screen, right third, bottom half | `AUX15` | `+melee` | Knife |
| Screen, middle third, top third | `KP_5` | `chatmodepublic` | Chat: opens the on-screen keyboard ("Typing") |

- **Moving** is four keys, so it is full speed in eight directions. A
  direction takes hold at 40% of the stick's travel past the dead zone and
  lets go at 30%.
- **Looking** goes through the engine's mouse path, so `sensitivity`, pitch
  inversion and the slowdown while aiming apply. At full deflection the stick
  sends `vita_lookSpeed` mouse counts a second: 1400, which is about 150
  degrees a second at the default sensitivity. Up and down is slower
  (`vita_lookPitchRatio`), and the response is a curve (`vita_lookCurve`), so
  a small push turns slowly.
- **Touch.** A finger keeps the area of the screen it landed in until it
  lifts, wherever it slides. The outer thirds are where the thumbs reach.
  The top third of the middle is a key too, small and out of the thumbs' way
  so that a keyboard does not open in a fight; the rest of the middle is no
  key. Team chat has no area: bind `chatmodeteam` to a control to have it.
- **The rear pad** is a tap. The fingers that hold the console lie on it: the
  first console run logged 30 touches in 40 seconds where two taps were asked
  for. So a finger that stays longer than 0.3 s is resting and does nothing. A
  tap presses its half's key when the finger leaves, and the key stays down
  while the left stick is pushed forward and for a quarter of a second after.
  With the default binding that is the console game's sprint: tap, and run
  until you stop. The key also stays down while L is held, and the same
  binding holds the breath behind a scope: aim with L, tap, and the breath is
  held until L is let go. That follows the button, not the action, so it
  assumes aiming stays on L. A tap while standing still without L is a short
  press, and a tap while the key is down presses it again.

### In a menu

- A finger on the screen moves the cursor there and clicks. The click is sent
  after the engine has been told where the cursor is.
- The left stick moves the cursor. Cross clicks.
- The d-pad is the arrow keys, which move the menu's focus, and repeat while
  held. It hides the cursor, which would otherwise take the focus back; the
  stick or a finger brings it back. While the cursor is hidden, Cross is
  Enter: it accepts the focused item.
- Circle and Start are Escape. The right stick is the mouse wheel.
- When a menu opens or closes, the sticks' and the panels' keys are let go,
  and a finger already on the screen does nothing until it lifts. A button
  keeps the key it pressed until it is released.

### Binding

The controls menu works: while it waits for a key, every control sends its
game key, and Start cancels.

The menu and the game's hints ("Press ... to use") name the controls: "R
button", "Square", "Rear touch (left)", "Screen (top right)". The shoulder
buttons are "L button" and "R button" there because the menu lists a profile's
PC keys beside them, and its plain "R" is the keyboard's (run KB1 showed
"Left Mouse or R" for firing and "R" for reloading). The engine keeps two
tables of key names, one for `bind` and the config files and one for what it
shows. Only the second is changed, when the input starts, so configs still
say `AUX6` and read the same on a PC. The engine shows a key's name as it is
when the game's texts have no entry under it, which is what makes this work
without an engine change. `vita_buttonNames 0` shows the engine's names.

The controls menu is the PC game's and has a row only for the PC's actions.
Square's `+usereload`, the console game's one button for both, has none: the
"Use" and "Reload Weapon" rows are `+activate` and `+reload` and show the
profile's PC keys alone (run KB1: "F" and "R"). A row shows two keys at
most, so the rear pad's second half is not named beside "Shift or Rear touch
(left)". Binding a control in a row works and replaces the control's default.

A profile from a PC, a new profile and the menu's "reset to default" all
leave the AUX keys bound to nothing. While none of AUX1 to AUX16 has a
binding, the default layout is set (`vita_defaultBinds`, on by default; the
log says so). One control unbound on purpose stays unbound. `vita_binddefaults`
sets the whole default layout at any time.

The default layout can grow. A profile's `vita_layout` says which default
layout its controls were last given: 1 is the first, and a config that does
not say is 1. A control that came with a later layout gets its default action
once in an older profile, if it is bound to nothing there, and the profile is
then marked with today's number (2: the chat area). Nothing the player has
bound is changed. To change the defaults, edit `kControls` in
`vita_input_map.cpp`; a new control gets the next layout number there.

| Dvar | Default | |
|---|---|---|
| `vita_stickDeadzone` | 0.2 | Part of a stick's travel around the centre that does nothing |
| `vita_lookSpeed` | 1400 | Right stick at full deflection, in mouse counts a second |
| `vita_lookPitchRatio` | 0.65 | Up and down, as a part of `vita_lookSpeed` |
| `vita_lookCurve` | 2 | 1 is linear; higher is finer near the centre |
| `vita_cursorSpeed` | 700 | Left stick in a menu, pixels a second |
| `vita_frontTouch` | 1 | The screen's four areas are keys in the game |
| `vita_rearTouch` | 1 | A tap on either half of the rear pad is a key in the game |
| `vita_defaultBinds` | 1 | Set the default layout while no AUX key is bound, and give an older profile the controls that came since |
| `vita_layout` | 1 | The default layout the profile's controls were last given (kept in the profile) |
| `vita_buttonNames` | 1 | The controls menu and the hints name the Vita's controls, not the engine's keys |
| `vita_keyboard` | 1 | Open the on-screen keyboard when a menu's text field or the chat line waits for text |
| `vita_keyboardAnswer` | empty | For a run with nobody to type: no keyboard is shown, and every keyboard asked for is answered with this text after half a second (not kept in the profile; read at start-up only, so a game server cannot set the text that gets typed) |
| `vita_inputDebug` | 0 | Log every key sent, every finger that lands (with its panel and screen position) and how long it stayed |

### Typing

When the game waits for typed text, the Vita's own on-screen keyboard opens
(the system's IME dialog), and what was typed goes to the game as the
characters and the Enter a PC keyboard would send.

- **A menu's text field** (a profile name, a server address, a password):
  touch it, or move to it with the d-pad and press Cross. The keyboard opens
  with what the field holds. Enter replaces the field's text and ends the
  edit; closing the keyboard without Enter leaves the field as it was.
  Number fields get the number keyboard. A field that a menu puts into
  editing by itself opens the keyboard by itself: the "create profile" box
  does (run KB1), so a first start opens on a keyboard for the name.
- **Chat:** a touch on the top middle of the screen opens the chat line and
  the keyboard. Enter says it; closing the keyboard closes the chat line.
- **A console command:** the command `vita_typecommand` opens the keyboard,
  and what is entered runs as a command. It has no control by default; bind
  it, or put it in the command line file.

How it is built:
- The keyboard is the system's, drawn over the game's frame. So it needs the
  frame: `gfx_gxm::SetBeforeFlip` (`gxm_display.h`) calls back on the thread
  that draws, between a frame's last scene and its flip, with the back
  buffer, the depth buffer and the sync object, and `sceCommonDialogUpdate`
  runs there. Every call of the dialog is made on that thread
  (`vita_keyboard.cpp`); the main thread asks for a keyboard and takes the
  result through a small mailbox.
- The game runs on under the keyboard. While it is up the controls belong to
  it: the pad and the panels are read and nothing is sent. A button or a
  finger that is still down when it closes does nothing until it is let go,
  so the touch that pressed the keyboard's Enter does not click the menu
  under it (in Vita3K it did: run KB1).
- One keyboard for each wait: after it closed, the game has to stop waiting,
  or wait in another field, before another opens. If the field goes away
  while the keyboard is up (the menu closed, the match ended), the keyboard
  is closed and its text dropped.
- The engine's text is one byte a character. Letters outside ASCII and
  Latin-1 are left out of what was typed.
- If the system refuses the keyboard, the log says so, none is asked for
  again in that run, and a chat line that opens is closed again. With no
  keyboard (refused, or `vita_keyboard 0`) and no player profile, the "create
  profile" box is offered the name `Vita` to accept, as before the keyboard.
- A run nobody watches should start with `+set vita_keyboard 0` if a text
  field may take the focus: an open keyboard waits for someone. Or with
  `+set vita_keyboardAnswer <text>`, which answers every keyboard asked for
  with that text and shows none; the log then has the "keyboard asked for"
  and "keyboard closed" lines without `vita_inputDebug`. That is how a run
  checks what the game does with entered text. It does not go through the
  system's dialog.

**Run so far.** The host tests cover when the keyboard opens and what the
game gets. Two runs in Vita3K on the Windows PC (2026-11-10, builds made on
that PC, by hand):

- **KB1** (a build of 9625bbad): the keyboard was asked for by
  `vita_typecommand` at start-up, by the "create profile" box's name field (by
  itself and again on a touch), and by the chat line after a touch on the top
  middle of the screen in a game; closing it with and without Enter was told
  apart each time; an older profile got the chat area's binding once. The
  click that closed Vita3K's dialog also clicked the game, which is what the
  resting controls are for.
- **KB2** (a build of df2902fa, with `vita_keyboardAnswer`): a typed command
  ran; a menu field (the server name, 20 characters at most) took the text
  and left editing; the chat line was said, and the log has it. The controls
  worked after each. Without the switch, the click that closed Vita3K's
  dialog was no longer a `MOUSE1` press. The controls menu read "Left Mouse
  or R button" and "L button".

No error in either, and every start ended by itself. Vita3K draws a dialog
of its own, its `sceCommonDialogUpdate` does nothing, and its text field took
no scripted typing, so **no entered text has come back from the system's
dialog yet**, in Vita3K or anywhere. Also not run: everything on a console
(the dialog drawn into the frame, which thread may call it, the memory it
takes beside a loaded map, and whether a console hands the game any touch
while the keyboard is up).

Run KB2 also showed the game's chat font drawing no underscore, and the
profile's `config_mp.cfg` keeping its old length when a shorter one was
written, which left a stray last line. That is Vita3K: the port opens the
file to be cut and a console cuts it, and Vita3K's file layer ignores the
flag (the Vita client thread's reading of both sources).

### Not done yet

- Walking speed from how far the stick is pushed. The engine moves by keys.
- A stray touch on the rear pad that is as short as a tap still counts as
  one. `vita_rearTouch 0` turns the pad off. Run K's log, from the menus where
  the pad is no key, lists 16 rear touches: 14 stayed longer than 0.3 s (up to
  71 s), which the tap rule ignores, and two were as short as a tap (152 and
  205 ms). So a hand that only holds the console sends a stray tap now and
  then. The tap itself has not been tried in a game; that run will say whether
  it needs a tighter rule.

## Sound (stage 3)

The engine's sound driver (`src/sound/snd_openal.cpp`,
`snd_driver_openal.cpp`) talks OpenAL 1.1 with a few EFX calls and three
extensions, 47 entry points in all. The Vita client answers them itself:

- `src/vita/client/sound/vita_al.cpp` is a small OpenAL on an integer mixer.
  It uses no Vita call and no engine header, so
  `scripts/tests/test_vita_sound.py` builds it on the host under
  AddressSanitizer, UBSan and ThreadSanitizer.
- `src/vita/client/sound/vita_al_output.cpp` opens a SceAudioOut port and runs
  the thread that pulls the mixer.

OpenAL Soft was the other choice. Its float mixer, HRTF and reverb cost more
of the Vita's CPU than sound should, and it would be one more dependency to
build for the Vita. The engine needs little of it.

What the small OpenAL keeps:
- sources with one buffer or a queue of buffers, the four source states,
  looping, gain, pitch and the play offset;
- the buffer and queue rules the engine depends on: a buffer in use cannot be
  deleted, a playing source cannot change its buffer, only played buffers
  leave a queue;
- mono and stereo buffers of 8 or 16 bits at any rate, resampled with linear
  interpolation;
- IMA ADPCM buffers (`AL_EXT_IMA4`, with the block's frame count from
  `AL_SOFT_block_alignment`), which stay packed and are decoded a block at a
  time as they play, and loop points (`AL_SOFT_loop_points`) for a looping
  source on one buffer. The next part says what they are for.

What it leaves out:
- **Distance models.** The engine sets `AL_NONE` and puts its own falloff
  into the gain.
- **The listener.** The engine keeps it at the origin and hands every
  position in listener space, so a position only pans, left to right with
  equal power.
- **Reverb and filters.** Effects, filters and slots get names and nothing
  else, so there is no reverb.

The mixer adds every playing source into 32-bit sums and clips to 16 bits.
Gains are Q15 and move toward their targets every 64 frames, so a volume or
position change does not click. One lock guards all of it; the mixer holds it
for one update.

**Loaded sounds in memory** (`snd_adpcm`). The OpenAL driver keeps every
loaded sound as 16-bit PCM, and that is also how the fastfiles hold the sounds
counted so far: all 702 the menu loads and the nine Crash adds came as 16-bit
PCM, none as IMA ADPCM, which the loader reads too. On the Vita they were most
of the heap: the menu's sounds took about 40 MB of the 64 MB (console run K:
55 MB in use at the menu, 60.7 MB when a map load failed), and a start that
ran out of heap crashed in `SND_SetData`, which wrote through the null
pointer.

With `snd_adpcm 1`, the default on the Vita, the driver compresses loaded
sounds to IMA ADPCM when they load, four bits a sample where the game's files
have 16, keeps them in whole blocks (`src/sound/snd_ima_adpcm.h`) and lets
OpenAL decode them as they play. The retail game played these sounds
uncompressed, so this is a loss in quality taken for the memory:

- A sound loaded as IMA ADPCM at a rate the mixer plays keeps its own blocks,
  byte for byte.
- Every other sound is encoded: PCM, and IMA ADPCM above the mixer's rate,
  whose rate is halved first as it always was. The blocks are 505 frames, 256
  bytes a channel. Encoding is lossy. In the tests a low tone comes back about
  50 dB above the encoder's noise and a 2 kHz tone at 22 kHz about 27 dB.
- The small OpenAL holds the packed bytes and one decoded block for each
  buffer (1 KB for a mono sound's 505 frames). It decodes a block when a
  source plays into it; a source at zero gain decodes nothing. In the tests a
  packed buffer mixes to the same samples as a PCM buffer of its decoded
  frames, at any pitch, looping, from an offset and in a queue.
- A sound that ends inside its last block loops at its own end through loop
  points. Played once, the rest of the block follows as silence, under 23 ms.

`snd_adpcm` is read when the sound system starts, and `+set snd_adpcm 0`
brings the 16-bit PCM back, to compare by ear or by memory. An OpenAL without
one of the three extensions gets 16-bit PCM and a warning. The desktop builds
leave `snd_adpcm` off; with it on, the same driver code plays through OpenAL
Soft, which `scripts/tests/test_openal_packed_sounds.py` checks on its
loopback device. `dumpraw` writes sounds as they are stored, so dump them with
`snd_adpcm 0`.

A sound that finds no memory when it loads is now left without data and does
not play. The log has a warning for the first four and a count in the line
below.

After each burst of loading, with `snd_adpcm`, with `snd_alDebug 1` or when a
sound found no memory, the log has one line: `sound data: ... sounds loaded,
in ... KB (... KB as 16-bit PCM); they came as ... IMA ADPCM, ... 16-bit and
... 8-bit; ... kept as IMA ADPCM, ... encoded to it in ... ms, ... found no
memory, ... claimed more frames than their bytes hold`. The milliseconds are
what the encoding added to the load. They are whole milliseconds of the clock
summed over sounds that take about one each, so read them as a rough total.

Some sounds claim more frames than their bytes hold. Both ways of keeping a
sound now read no further than its bytes. Before, the 16-bit path believed the
claim when it halved a sound's rate, read past the end of the sound's data and
kept what it found there as the sound's second part. That happens only below
`snd_khz 44`, so the desktop's default never met it. The last figure in the
line counts such sounds, and `snd_alDebug 2` prints the first eight: `sound
data: a sound claims ... frames and its ... bytes hold ...`.

Measured in Vita3K on the Windows PC (run S, 2026-11-10, a build made on that
PC, at the menu, `snd_khz 22`; the log line had no milliseconds or claim count
yet):

| | `snd_adpcm 1` | `snd_adpcm 0` |
|---|---:|---:|
| The menu's 702 sounds | 9574 KB | 40882 KB |
| The same sounds as 16-bit PCM, by the log line | 37405 KB | 40882 KB |
| Heap in use at the menu, of 64 MB | 23.8 MB | 55.0 MB |

All 702 were encoded and none found no memory. Menu sounds started 22 times
from packed buffers with no OpenAL error, no cut and no clipped frame, and an
update of the mixer took 21 to 24 µs on average. With Crash's fastfile started
the count was 711 sounds in 9693 KB. The two columns disagree on the second
row by 3477 KB: that is what the 16-bit path read from past the ends of
sounds, as above.

Run T (the same PC and Vita3K, the PC's build of the fix, 2026-11-10) has the
fix's figures. With `snd_adpcm 0` the 702 sounds take 37405 KB by both figures
of the log line, and the heap at the menu holds 51.3 MB. With
`snd_adpcm 1` nothing changed: 9574 KB and 23.8 MB. Encoding the 702 sounds
took 1232 ms in the emulator. 26 of them claim more frames than their bytes
hold, and every one the log printed is a 16-bit stereo sound at 44100 Hz that
claims exactly twice its frames, so a stereo sound's frame count seems to
count both channels. Where sounds are not halved (the desktop's default) the
driver worked out such a sound's length from that count, so it counted as
twice as long as it plays; the copying path now lowers the count to the frames
the bytes hold, as the other two paths did. Nobody has listened for a
difference on the desktop. Run U loaded Crash: 749 sounds in 12184 KB, where
16-bit PCM would take 47675 KB, and the heap in the game held 32.9 MB.

Not measured yet: all of this on a console (Vita3K's memory figures are not a
console's), what the encoding adds to a load there, the path that keeps a
sound's own IMA ADPCM blocks with real data, and how the encoded sounds sound.
Nobody has listened to them: the runs had no listener.

**Output.** The engine asks for 11025, 22050 or 44100 Hz (`snd_khz`). The
default is 44, and 22 on the Vita, where a saved config still wins: at 22 the
engine halves loaded sounds above 33075 Hz when they load, and at 44 it halves
none, so they take up to twice the memory. SceAudioOut's main ports play 48000 Hz only, so the client opens the one
background-music port, which takes the engine's rates, and mixes at the rate
asked for. If that port does not open, it takes a main port and mixes at
48000 Hz. One update is about 23 ms (512 frames at 22050 Hz, 1024 at 44100),
and `sceAudioOutOutput` blocks until the port has taken it, which paces the
thread.

**In the log** (`console-mp.log`): `vita sound: port ..., ... Hz stereo, ...
frames per update` when sound starts, then after ten seconds, with the
engine's other figures (once a minute by default, `vita_statsEvery`) and once
more when sound stops `vita sound: ... updates, mixing ... us each on
average and ... at most, ... sources at most, ... clipped frames, ... refused
updates, ... packed frames decoded, buffers ... KB at most`.

**On the console** (run 1, 2026-11-09): the menu music and the menu's sounds
were heard. The port opened at 22050 Hz with 512 frames per update (that
console's `snd_khz` is 22). An update with nothing playing took 34 µs on
average and 89 at most. What an update costs with sources playing was not
captured: the run ended before the second line, and the line at the end came
after it.

Run K (2026-11-10, the menu on the screen, with its music and sounds): two
sources at most, an update took 39 to 53 µs on average and 220 at most, and no
frame clipped and no update was refused. The music paused whenever the menu
stopped responding, for up to 23.6 s while shaders compiled, and came back
each time. The mixer thread ran through those pauses; it had no music to play,
which is the first limit below.

Known limits:
- The engine decodes streamed sounds (music) on its main thread, 8192 frames
  at a time. When the main thread stalls, the music stops until it is back
  (run K, above). In a game the decoding may also show as a frame hitch; a run
  in a game will tell.
- The background-music port is the one the system's own music player takes
  over when the user plays music from the home screen.

## Graphics (stage 2)

The renderer talks D3D9. On the PC, `r_backend vulkan` answers it with the
Vulkan work's D3D9 front (`src/gfx_vk/d3d9`) on the Legacy adapter. The Vita
client uses the same front, on a second implementation of the Legacy API:
`src/gfx_gxm/legacy/gxm_legacy.cpp`, on sceGxm.

The front reaches its GPU only through `src/gfx_vk/d3d9/vk_d3d9_backend.h`,
when it creates and destroys a device. That seam has two implementations:
- `vk_d3d9_backend_vulkan.cpp`: a Vulkan core per device;
- `src/gfx_gxm/legacy/gxm_d3d9_backend.cpp`: the Vita's one screen.

Builds with `KISAK_GFX_GXM` leave the Vulkan parts of `vk_d3d9.h` and
`vk_legacy.h` out.

What the GXM back end does so far (step 2d):
- The device owns the screen and reserves the GPU memory pools: nearly all of
  CDRAM, 20 MB of main memory, and an 8 MB parameter buffer.
- Textures, surfaces and buffers live in those pools with D3D9's lock rules.
  Textures go to CDRAM first, dynamic buffers to main memory first.
- Shaders are translated to Cg and compiled on the Vita (below).
- Clears and draws happen in sceGxm scenes (`legacy/gxm_legacy_draw.cpp`,
  below). Present ends the scene and shows the back buffer with vsync.
- StretchRect draws a textured quad into the destination.
- Locks of the back buffer, depth textures as shader inputs,
  two-sided stencil and the blend factor colour are logged once as not done
  yet.

### Draws (step 2d)

`src/gfx_gxm/legacy/gxm_legacy_draw.cpp` turns the D3D9 state into sceGxm's:
- **Scenes.** A scene opens on render target 0 and the depth-stencil surface
  at the first clear or draw after they change, and closes when they change,
  before the CPU reads a target, and at Present. sceGxm wants a render target
  object per surface size; `gxm_display` keeps one per size. Depth-stencil
  surfaces (tiled S8D24) load and store their contents every scene, so a
  frame can come back to a target.
- **StretchRect** (the renderer's resolves and cinematic frames) draws the
  source, the back buffer or a texture level, as a textured quad into the
  destination with two more built-in programs. One that cannot happen (no
  GPU memory for either side) is counted as a skipped draw.
- **Clears** are quads drawn in the scene with two small built-in programs:
  colour by the fragment program, depth and stencil by an always-pass depth
  test. sceGxm has no clear call, and fills on the transfer queue would race
  the scenes. A clear that keeps the colour (depth and stencil only) draws
  with the fragment program switched off
  (`sceGxmSetFrontFragmentProgramEnable`), which writes no colour on any
  target format. It used a colour mask until console run L: a mask needs a
  program made with blend info, and a console refuses that for a float
  target (`SCE_GXM_ERROR_UNSUPPORTED`, 0x805b0009) where Vita3K makes it.
  The renderer clears depth and stencil that way before every float-Z
  pass, so on the console no frame with depth of field or soft particles
  had its depth cleared ("Run L" under "Memory").
- **A scene's colour is cleared before it is drawn.** `r_clear` is `steady`
  (3) in the Vita build and the retail game's `dev-only blink` (1)
  elsewhere, so the renderer clears the scene to `r_clearColor`, black,
  every frame. The retail game clears the colour only on a frame without a
  float-Z pass, because its scene covers every pixel. The Vita leaves a
  draw out while its program is being made ("Programs in the background"),
  and without the clear the hole shows what that display buffer held three
  frames before. `+set r_clear 1` gives the retail behaviour back.
- **A program sceGxm refuses to make** is logged once, with sceGxm's error
  (`draw skipped: the fragment program could not be made: ...`), and every
  draw or clear that asks for it afterwards is counted in the summary
  (`draws skipped: N x ...`). Logs from before 2026-11-10 counted only the
  first: a `1 x` there means one program, and it may be a clear that never
  happened.
- **Programs.** Each draw picks its shaders' variants: the vertex
  declaration decides which inputs are D3DCOLOR (swapped), missing (read as
  0, 0, 0, 1) or narrower than four components, and the alpha test picks the
  pixel shader's. A variant other than the default is asked for on first
  use and made on the cache's thread; the draw is left out until it is
  ready, then it is registered with the shader patcher. Vertex
  programs are kept per variant, declaration and stream strides; fragment
  programs per variant, vertex program, blend state and target format.
- **A draw does not do again what an earlier one did** (built, not run
  yet). Before, every draw built two map keys as strings to find its
  programs, made each texture's view with five `sceGxmTextureSet...` calls,
  set every texture unit and copied its vertex shader's whole constant
  array (up to 4 KB) into a newly reserved uniform buffer. Now:
  the programs a combination of shaders, declaration, strides, alpha
  function and index width drew with are remembered in a table of 512 and
  forgotten when a shader or a declaration goes; a texture keeps its view
  for the filters and address modes it was last sampled with; a unit that
  already holds the same texture in the open scene is not set again; and
  the uniform buffers are neither reserved nor written when both programs
  and every constant are the last draw's (sceGxm keeps a reserved default
  uniform buffer for a program until the program is set anew; the back end
  drops both when either program changes and at every scene's start, and
  always writes for a pixel shader that takes a volume texture's depth).
  Draws a run diagnostic looks at or changes take the old path. The quit
  summary says how often each applied: `draw work left out: N of N draws
  found their programs in the table; of N texture binds N were set already
  and N set their filters anew; of N uniform buffers N were the last
  draw's`. A file named `no-draw-reuse` in the folder of the compiled
  programs turns all three off (the log says so at start-up), so that a
  console run can tell a fault of theirs without another build. Console run
  N (2026-11-10, mp_crash) measured them: the back end's draw time fell from
  28 to 30 ms a frame to 13 to 17 ms, but frames took as long (130 to 165
  ms), since the console waits 80 to 110 ms a frame for the GPU there.
- **Where the GPU's time goes.** A file named `gpu-scene-times` in the same
  folder makes the CPU wait for the GPU at the end of every scene and add
  the wait to that scene's targets. Every 60 frames the log gives the GPU's
  time a frame by target, most first: `diag: gpu by scene: N ms a frame, N
  scenes and N draws a frame: <colour target> + depth <depth target> WxH`.
  The back buffer's scene also holds any wait for the display to let the
  buffer go. The waits stop the CPU and the GPU working at once, so frames
  are slower with it; it is for finding the costly pass, not for frame
  rates.
- **Textures** are linear: rows of whole multiples of 8 texels, and the
  levels of a mipmapped texture sized from the power of two above the base,
  which is how sceGxm reads them and also a layout LockRect can hand out.
  Render-target textures are strided (rows of 32 texels, as colour surfaces
  want). Volume textures are their slices stacked. sceGxm samples cube
  textures only swizzled, and refuses linear DXT textures (run D:
  `sceGxmTextureInit... failed (0x805b0009)` for every DXT1/3/5), so cube and
  DXT textures are swizzled: the GPU holds only the swizzled copy, LockRect
  hands out a scratch level in D3D9's layout (filled from the GPU's copy for
  partial or read-only locks), and UnlockRect swizzles it in. DXT textures
  whose sides are not powers of two use sceGxm's arbitrary swizzled layout,
  padded to the powers of two. Depth textures are not sampled yet: draws
  bind a white texture.
- **State.** The viewport moves half a pixel (D3D9 samples pixel centres at
  whole coordinates); the viewport and scissor rect become the region clip;
  depth, stencil, cull, fill mode and depth bias map to sceGxm's; blending
  goes into the fragment program.
- **Memory.** DISCARD on a buffer a draw read takes fresh memory. Memory the
  GPU may still read (old buffers, destroyed resources, released programs) is
  freed three frames later, since the display queue lets the CPU run at most
  two frames ahead. When a new resource finds no room, the back end waits for
  the GPU, frees all of that memory at once and tries again, logging
  `no room for ...: waited for the GPU and freed N retired pieces`. A map
  change needs this: it frees the old map's buffers and asks for the new
  map's before a frame is shown (run Z8's long warm-ups ran out of CDRAM
  going from mp_crash to mp_backlot). DrawPrimitiveUP and the clear quads
  copy their vertices into a 512 KB per-frame ring.

### Shaders (step 2c)

`src/gfx_gxm/shader/gxm_cg_translate.cpp` turns D3D9 shader bytecode
(vs_1_1 to vs_3_0, ps_2_0 to ps_3_0) into Cg: a float4 local per register and
one statement per instruction. Loops, calls, predicates, `if` on boolean
constants, `vPos`, `vFace`, more than one colour output and ps 1.x are
refused with a reason; the game's shaders use none of them (the renderer
plan's corpus scan). GXM has no volume textures, so a volume is stored as its
slices one under the other in a 2D texture and sampled by blending the two
nearest slices (`kisak_tex3D`, slice count in `kisak_depth_s<n>`). The Cg
program meets the back end through:
- the uniform array `c` (float constants), as long as the highest one the
  shader reads from the API, or 256 when it indexes them with `a0`;
- the samplers `s0` to `s15`, and vertex inputs `in_v0` to `in_v15`, by name;
- varyings by GXM semantic: texcoord n is `TEXCOORDn`, color n is `COLORn`,
  fog `FOG`, point size `PSIZE`;
- options for the draw state D3D9 keeps outside the shader: vertex inputs in
  D3DCOLOR byte order (swapped back with `.zyxw`), and the alpha test, which
  GXM does not have (`kisak_alpha_ref` and a discard).

`src/gfx_gxm/gxm_program_cache.cpp` keeps each compiled program in
`ux0:data/KisakCOD/gxp/<hash>.gxp`, named by a hash of its Cg source, so a
console compiles a program once (about 0.7 s each for the game's programs
on a console; the probe's small ones took 0.1 s). A program that does not
compile is logged with the compiler's first error (the first 40 failures;
each reason a shader is not translated is logged once and counted at the end)
and its Cg is left beside the cache as `failed-<hash>.cg`. A shader that
cannot be translated or compiled is still created, as D3D9 would; draws with
it are skipped and counted.

The compiler is loaded when the first program has to be compiled, so a start
that finds every program on the card never loads it. It allocates from the
game's heap and keeps working memory between programs, so the cache unloads
it once it holds more than 2 MB of the heap and once it has compiled nothing
for 5 s, and loads it again for the next program (0.4 s on a console; vitaGL
does the same). A first start compiled 421 programs across 15 loads in
Vita3K, with the compiler never above 4.1 MB; before that, such a start ran
the heap out while the sounds loaded. The `shader compiler memory` log lines
count what it held and gave back. In PC run Z11 (Vita3K) the compiler
stopped on the first program of a start with `error 403, line 0: fatal
internal error`, failed the next 278 the same way within a fifth of a
second, and then made nothing more for ten minutes. A program that fails to
compile is now tried again with the compiler unloaded and loaded afresh; a
fragment program that still fails gets a last try with fast maths, as it was
built before run Z10; and a program that fails every try leaves the
compiler unloaded for the next. The same lines count the retries.

**Programs in the background.** The game creates its shaders while a zone
loads: on its loading thread, hundreds in a row, inside the lock every D3D9
call takes (`FrontLock`, `src/gfx_vk/d3d9`). `common_mp` loads behind the
menu, so the menu's frames want that lock too. Up to run K each shader's
program was read from the card or compiled right there, 24 ms or 0.5 to
1.4 s with the lock held, and the lock went straight back to the loading
thread for the next shader: a run of new shaders stopped the menu, and the
music the main thread feeds, for as long as the whole run took (23.6 s for
about 23 programs in run K).

Now creating a shader only translates it (1.6 ms on a console) and asks for
its program. `src/gfx_gxm/gxm_program_queue.cpp` makes the programs on one
worker thread, in the order asked, through the same cache; the compiler is
loaded, used and unloaded on that thread. A draw whose vertex or pixel
program is not ready is left out of that frame and moves its program to the
front of the queue, so what is on the screen is made first and nothing waits
for the card or the compiler. A shader destroyed before its turn is dropped
unmade. The back end's few built-in programs are still made at start-up,
before the first frame. What to expect: on a first start the menu fills in
over the first seconds instead of freezing, and compiling goes on quietly for
some minutes; later starts read the programs from the card in the
background. Log lines: `programs in the background so far: N asked for, N
waiting now (N at most), N hurried by a draw, N dropped unmade; slowest
compile N ms, slowest read N ms; N draws left out of N frames while their
program was made`, and `long frame N: ...` for any frame over 250 ms (the
first 60), with the shaders created, texture levels filled and programs made
meanwhile. "programs ready" in the `shaders` line now counts every program
the cache made, variants and built-in ones included. The queue itself makes
no Vita call and is checked on the host with the address and thread
sanitizers (`scripts/tests/test_vita_program_queue.py`). Music still stops
in any other stall of the main thread longer than the 0.7 s it queues ahead
(a map load); that needs the stream fed from another thread.

In Vita3K on the Windows PC (run H1, 2026-11-10, c5a28eba merged with master
6e11f2b3; an emulator's timing, not a console's): a start with an empty cache
drew its first frame 6.9 s after the launch, where the build before drew it
after 35.2 s, and its slowest frame was 484 ms, where the build before had
frames of 19.2, 8.5, 27.1 and 17.9 s. The 421 programs compiled in 267 s in
the background (as long as before), 1209 draws were left out of 34 frames,
and the window showed the whole menu 44 s after the launch. A start with the
cache filled compiled nothing, read 837 programs in the background in 1.3 s
and left 8 draws out of 2 frames. A map start (mp_crash) whose cache held
only the menu's programs loaded and drew the world while 238 programs
compiled: 70165 draws were left out of 147 frames, none after the first half
minute in the map. Not yet run on a console.

**The compiler folds `x * 0 + 1` to 0.** HLSL writes `float4(v.xyz, 1)` as
one multiply-add with the constant (1, 0, 0, 0): `mad r0, v0.xyzx, c8.xxxy,
c8.yyyx`. Written to Cg as `r0 = v0.xyzx * k8.xxxy + k8.yyyx`, the Vita's
compiler drops the w lane's 1, at every optimisation level but 0 and with or
without fast maths. Every position then lost its w terms, which left the
screen black until run K. The translator now takes the lanes whose factor is
a constant zero out of the product (`WithoutZeroProducts`): they take the
addend, or 0.0 for a plain multiply. Literals in place of the constant and
one component per statement also compile right; a non-const or static
constant, two statements, a function, and constructors around the swizzles
do not.

**It gets other lanes of swizzled constants wrong too.** In PC run Z7
(Vita3K, 2026-11-10, mp_crash) the world's decals drew red and green that
ignored their colour map (posters on a wall came out flat yellow; with
black colour maps Windows drew them black and the Vita 145 135 0), while
blue was right. Compiled at optimisation 0 the posters' two programs drew
them as Windows does. The translator now writes every def'd constant it
reads as a literal of the components the swizzle picks
(`float4(1.0, 1.0, 1.0, 0.0)` for `c8.xxxy`), never as the constant and a
swizzle. That changes the text, and so the cache file, of nearly every
program: the first start after it compiles them all again (about 1200 on
mp_crash), in the background as usual.

**Fast maths breaks the specular decals.** With the literals the
posters were right, but the ground under Crash's decals still showed a fine
red and green speckle (PC run Z8). It needed both the normal map and
specular lighting, and black specular maps did not remove it on the Vita
(run Z9). The two specular decal programs add a reflection made of the
cube map times the specular map times the vertex alpha times a Fresnel
factor; the specular map's alpha also picks the cube's level. Built at the
usual optimisation level with fast maths off, they drew the ground as
Windows does, within 1 (run Z10). Fragment programs are therefore built
without fast maths; vertex programs keep it. The cache key of a fragment
program changed with it, so its first start compiles them again. What fast
maths off costs on a console's GPU is not measured yet.

The client is packed unsafe for the compiler, like the probe.

`scripts/tests/test_vita_cg_translate.py` checks the translator on the
host: the shader spike's 81 op-by-op cases are translated (58) or refused (23) as
expected, glslang's HLSL front end accepts every program (Cg and DX9 HLSL
share the syntax the translator writes), a few outputs are checked by hand,
and damaged bytecode is refused without a crash (ASan and UBSan on). Only the
Vita's own compiler is the real check, and it runs in the client.

## GXM probe (stage 2, step 1)

`KisakCOD-gxm-probe.vpk` (title ID `KSKD00003`) is built with the client. It
checks the graphics layer the client will stand on, on its own and in a few
seconds, before the renderer runs on it. It uses no engine code and no game
data. The source is `src/gfx_gxm/tools/gxm_probe.cpp`.

What it does, in order:
1. Brings up sceGxm, the GPU memory pools, three display buffers, a
   depth-stencil surface and the shader patcher (`src/gfx_gxm/gxm_display.h`).
2. Loads the shader compiler from `ur0:data/libshacccg.suprx` and compiles six
   small Cg programs (`src/gfx_gxm/gxm_shader_compiler.h`), with their times.
3. Draws for about five seconds, with the time per frame:
   - a full-screen colour that cycles through the hues: the clear;
   - a spinning, blended triangle whose colours are D3DCOLOR bytes;
   - an orange and white checkerboard quad in the bottom right corner.
4. Clears the screen with `sceGxmTransferFill`, then shows a green band on the
   top third for a second.
5. Logs free memory after each step to `ux0:data/KisakCOD/gxm-probe.log`.

Without the shader compiler it skips steps 2 and 3, says so, and still runs
step 4. START + SELECT stops it early.

The probe and the client are packed **unsafe** (without `vita-make-fself -s`),
because safe homebrew cannot read `ur0:`. VitaShell asks for confirmation when
it installs them.

### The shader compiler

Each user extracts `libshacccg.suprx` once from their own console. It is part
of Sony's PSM runtime and can't be shipped:
1. Install ShaRKBR33D (Rinnegatamante's `.vpk`) with VitaShell.
2. Run it with Wi-Fi on. It downloads the PSM runtime and writes
   `ur0:data/libshacccg.suprx`.

The program links the compiler through weak imports (`SceShaccCg_stub_weak`),
so it starts without the file and reports that it is missing.

### GPU memory

`src/gfx_gxm/gxm_memory.h` reserves one kernel block per kind at start-up,
maps it for the GPU, and hands out pieces with a first-fit heap
(`gxm_heap.h`). The heap is plain offset arithmetic, so it is tested on the
host by `scripts/tests/test_vita_gxm_heap.py`. The kinds are:
- CDRAM: textures, render targets, the display buffers;
- uncached main memory: vertex data, ring buffers;
- the two USSE kinds: shader code.

The Legacy back end counts each pool's use by what it is for and logs it
with the engine's other figures (once a minute by default, `vita_statsEvery`,
and before a timed quit) and at every failed allocation (`gpu memory by use (cdram +
main MB, pieces): ...`), and logs each piece of 1 MB or more as it is made
(`gpu memory piece N: ...`, with its size, format and usage).

With the same figures, and once more when the device goes, it says where a
frame's time went since the last such line, as a frame's mean:
`gxm legacy: frame time, last N frames: X ms a frame. In this back end: N
draws X ms (one in 16 timed), N clears and copies X ms, N scenes' starts and
ends X ms, N waits for the GPU X ms (longest X ms), showing the frame X ms
(longest X ms). The rest, X ms, is the game, the D3D9 front and the filling of
buffers and textures. ...`. How to read it:
- **Waits for the GPU** are `sceGxmFinish` calls: a renamed buffer that found
  no room, a target read back. Each stops the CPU until the GPU has drawn
  everything sent so far.
- **Showing the frame** is `sceGxmDisplayQueueAddEntry`, which waits while
  every display buffer is still queued. A large figure there with small ones
  elsewhere means the GPU, or the screen's 60 frames a second, is what holds
  the frame.
- **Draws** are too many to time each (a clock read may be a system call, and
  the line ends with what one costs), so about one in 16 is timed, picked
  without a fixed step, and the sum is scaled up. A timed draw that took over
  2 ms (a program made on the spot) counts its excess once; the line says how
  many there were. Scene work and waits inside a draw, a clear or a copy are
  counted under their own names only.
- **The rest** is everything outside those calls. The system's `Vita load`
  line beside it says how busy the processor was.

The sums are plain arithmetic (`gxm_frame_time.h`), tested on the host by
`scripts/tests/test_vita_frame_time.py`. The line itself has not run on a
console or in Vita3K yet; in Vita3K its figures will be the emulator's.

The display's own buffers take about 9 MB of CDRAM and 2.6 MB of main memory.
The parameter buffer, which sceGxm takes from main memory itself, adds 16 MB.
The probe measures what is left.

## Stages

The plan, with reasons and risks: `vita-client-plan.md` in the project files.

1. **Build and boot** (this page).
2. **Screen:** a GXM back end for the Legacy adapter API (`src/gfx_vk/legacy/vk_legacy.h`), under the existing D3D9 front (`src/gfx_vk/d3d9`). Game shaders are translated to Cg, compiled on the Vita by `libshacccg.suprx` (each user extracts it from their own firmware) and cached as `.gxp`.
3. **Controls and sound:** buttons, sticks and touch become engine input, and sound gets a Vita output. Built and tested on the host ("Controls", "Sound"). Text is typed on the system's on-screen keyboard ("Typing"): it opens and closes in Vita3K, and a text given by a switch reaches the game there (runs KB1 and KB2); it has not run on a console.
4. **Join and play:** connect to a server and play, with the visual cuts from the feasibility study.
5. **Make it fast:** profile on the Vita, then cut the hot spots.
