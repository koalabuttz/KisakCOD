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
NEON float outside the vector skinning path, the static culling walk and the MP3 decoder, which all
have to be there and be NEON) and link checks (weak pthread references, the main stack size), and
uploads the RelWithDebInfo `.vpk`. It also runs the controls' and the sound's
host tests (`scripts/tests/test_vita_input.py`, `test_vita_sound.py`), the run
plan's, the GPU heap's, the frame time figures', the GPU progress bookkeeping's,
the GPU watch's, the idle alpha test's and the shader translator's
(`test_vita_run_plan.py`, `test_vita_gxm_heap.py`,
`test_vita_frame_time.py`, `test_vita_gpu_progress.py`,
`test_vita_gpu_watch.py`, `test_vita_idle_alpha_test.py`,
`test_vita_cg_translate.py`), the 3D picture size governor's
(`test_vita_scale_governor.py`), and the static model cache's size arithmetic
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

A Vita release build leaves the engine's assertions out, as retail and the
Windows release builds do: console run AS found them 5 to 8% of the frame at
the busy view. Configure with `-DKISAK_VITA_RELEASE_ASSERTS=ON` to keep them.

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
  heap instead of 60. (An empty value, the default, means 60; a `64` left in
  an older build folder's cache, the former default, is read as 60 too.)

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
- `clients` 1.7 MB, `g_streamStates` 0.4 MB (1.7 MB until 2026-11-15, when the sound driver kept an MP3 decoder's 33 KB of state for each of its 53 channels; only 13 of them stream).

The heap and the engine reservations are the server's for now: a 64 MB heap (60 MB in the client since the 16 MB parameter buffer), the 128 MB zone arena and the hunks. Textures go to the GPU's own memory (CDRAM, about 112 MB), not main memory.

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
- **A renamed buffer's copies stay in its own pool.** The engine locks most dynamic buffers with DISCARD every frame. The back end then gives the buffer fresh memory (a rename) and frees the old once the frames that might read it are done, up to three frames later, so a buffer can hold four times its size. Those copies used to spill from the main pool into CDRAM, where they took the room the scene's render targets needed. Now a rename takes memory from the buffer's own pool only (main, for dynamic buffers). With no room there, the lock ends the scene, waits for the GPU and writes in place, which costs time and no memory. One wait serves every buffer drawn before it: after it the GPU reads none of them, so until a draw reads one again its locks write in place without a rename or another wait. The draw summary counts all three: `buffer renames: N took fresh memory, N waited for the GPU instead (their pool was full) and N needed neither (a wait since their last draw); old copies the GPU might still read took N KB at most`. A refused rename no longer writes two lines to the log each time. In run V's 1356 frames 248 renames took fresh memory, 953 waited and 2013 needed neither, and old copies took 3072 KB at most. What the waits cost in frame time on a console is not known. (Later console runs answered that: one such wait in every frame, for the whole of the GPU's work on it, so that the CPU and the GPU took turns. The back end now keeps the pieces of memory and lets the two work at once; see "The CPU a frame ahead of the GPU" under the GXM back end below.)

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

**Streamed sounds' lengths.** The engine wants a stream's length when it
starts, and an MP3 without a Xing or Info tag does not say: the decoder reads
the whole file to count its frames, on the main thread. The game's streams
have no tag. Console run AY (2026-11-14, `Vita: long frame's calls`) caught
it: a 3.5 MB stream in `iw_11.iwd` started every minute or two of a match on
Crash, and each start held the main thread for about 0.75 s (0.42 s in 72
file reads, the rest counting), which is a frozen picture. The OpenAL driver
now keeps each file's count after its first play (`g_streamLengths` in
`src/sound/snd_driver_openal.cpp`, on every platform; host test
`scripts/tests/test_openal_stream_lengths.py`), so a stream is read through
once in a start of the game. `snd_alDebug 1` prints a line each time a file
is read through.

Console run BB showed that was a third of it: the repeat stalls went from
0.75 s to 0.48 s. The streams sit deflated in the `.iwd` files, where a seek
reaches a place by inflating up to it, and the MP3 decoder, given a way to ask
where it is in the file, looks for an ID3v1 or APE tag at the file's end when
it opens it: the seek to the end read the whole file and the step back to the
tag read it again. AY's 3.5 MB was one 1.2 MB file read three times. The
driver now opens a stream without that callback, so the open reads the file's
head only; a tag at the end of a file reaches the decoder as bytes that are no
frame, which it skips (and with them the last frame before the tag, 26 ms,
which the decoder cannot confirm against what follows). The same host test
opens a made-up MP3 both ways and counts the bytes.

That left the first play of each file, one read for the length: a quarter of
a second for a 1.2 MB file. The game's music is constant bit rate (128 kbit/s,
44.1 kHz), where every frame has the same length, so the file's size says how
many frames it holds. A file has to show that it is one, twice over. The
driver reads the first 32 KB of a file it has no length for: past an ID3v2
tag, every frame there has to be a layer 3 frame of one bit rate and sample
rate, each starting where the one before ends, and at least 16 of them. And
the file has to be a whole number of such frames long to the byte, alone or
with an ID3v1 tag's 128 bytes behind the last frame. Then the length comes
from the size, and nothing more is read. Any other file (variable bit rate,
as the ambient streams are; a tag longer than the head) is read through once
as before.

The head alone is not enough. A survey of the game's 119 MP3 streams on the
owner's PC (2026-11-14, `test_openal_stream_lengths.py --survey <folder>`)
found the 60 music files constant bit rate, each with an ID3v1 tag, and six of
the 59 variable bit rate ambient streams with one bit rate all through their
first 32 KB: their sizes gave lengths up to 40 s wrong. The second proof
turns five of the six away. The sixth is a whole number of frames long by
chance (about one file in seventy of another kind is) and gets a length 78 ms
out, for a stream that loops and so never uses it.

So a length from a size is put right at the first full play. When the decoder
reaches the end of a file it began at the first frame, the frames it gave
replace the size's answer (`AL_CountStreamLength`). For the music that takes
one frame off, 26 ms: the frame before the tag that the decoder does not
play. `snd_alDebug 1` prints which way each file went and each length put
right (`played through: F frames, not the G its size gave`), and level 2 also
reads a constant bit rate file through at its start and prints both counts.
The host test holds the size's answer against the decoder's own count on
made-up files, and `--survey` does the same for real ones.

**Streamed sounds on a thread** (`snd_streamThread`). The driver decoded a
stream where it queued it: on the main thread, 8192 frames at a time, each
time one of the stream's four OpenAL buffers had played out. On the Vita that
is 10 ms and more of MP3 decoding in one frame, every 186 ms for each stream
that plays: console run BM (2026-11-15) timed the sound update at 3.3 ms a
frame at the busy view of `mp_crash`, with single updates of 23 ms, on a
main thread that is busy 27 ms of a 37 ms frame. A stream that starts part of
the way in, as the ambient sounds do, also decoded its way to that place
before the start returned.

With `snd_streamThread 1`, the default on the Vita, a thread named `sound
decode` does that work. Each stream channel has a feed
(`src/sound/snd_stream_feed.h`): the thread decodes into it, two chunks
ahead, and the sound update only hands finished chunks to OpenAL. The thread
runs one priority step below the engine's threads, so it decodes when a core
has nothing else to do; the 0.7 s of sound a stream has queued is its margin.
What changes for the game:

- A stream's first sound is queued a frame or two after its start, when the
  thread has decoded it. The driver remembers that the source was told to
  play and plays it then (`AL_PlayStream`); a stream with nothing queued yet
  is not taken for one that has ended.
- The way to the starting place is the thread's as well. The MP3 decoder
  reaches a frame by decoding everything before it, which the feed does
  itself, a chunk's worth at a time between the other streams' chunks, so a
  stream that starts 100 s in neither holds up the streams that play nor a
  stop of its own channel.
- Stopping a stream waits until the thread is out of its decoder: a chunk is
  decoded in pieces of 2048 frames and the thread gives up at the next piece,
  so that is a wait of 3 ms at the most, and none most of the time.
- A stream's file inside an `.iwd` is always read through a file handle of
  its own (`FS_FOpenFileReadStream`). It used to read through the archive's
  one handle when no other file of that archive was open, and every later
  open in that archive seeks and reads on that handle from the main thread.

The MP3 decoder itself (dr_mp3) is built for NEON on the Vita: its synthesis
filter and transforms work on four floats at a time. Until 2026-11-15 it ran
on scalar VFP like the rest of the port, because the toolchain file's
`-mfpu=vfpv3` hid the CPU's NEON from it. The file it is built in
(`src/sound/snd_driver_load_obj.cpp`) gets `-mfpu=neon`
(`scripts/platform/vita/mp_client.cmake`); the samples are the scalar
build's to within one step of 16 bits
(`scripts/tests/test_mp3_neon_decode.py`, under `qemu-arm` in the Linux
workflow's armv7 job). Not timed on a console yet.

`snd_streamThread 0` gives the decoding back to the main thread, at once and
also for streams that are playing: the same feed is then stepped by the sound
update, a chunk at a time as chunks are wanted. That is the default on the
other platforms, where the decoding is too small to see. `snd_alDebug 1`
adds to its 5 second line how many chunks the thread decoded, how many the
main thread did, and how often a playing stream down to its last buffer found
none ready (`late`). `scripts/tests/test_snd_stream_feed.py` runs the feed and its
thread on the host under the address, undefined-behaviour and thread
sanitizers, and `test_openal_channel_stop.py` runs the driver's side against
OpenAL both ways.

The same change takes 1.3 MB off the client's static memory: the driver kept
decoder state for all 53 sound channels and now keeps it for the 13 that
stream.

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
  CDRAM, 30 MB of main memory, and a 16 MB parameter buffer.
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
  drops both when either program changes and at every scene's start).
  Draws a run diagnostic looks at or changes take the old path. The quit
  summary says how often each applied: `draw work left out: N draws kept
  the last draw's programs and fixed state; N of N others found their
  programs in the table; of N texture binds N were set already and N set
  their filters anew; of N uniform buffers N were the last draw's; of N
  draws with textures N had the last draw's and set none`. A file
  named `no-draw-reuse` in the folder of the compiled programs turns all of
  them off (the log says so at start-up), so that a console run can tell a
  fault of theirs without another build. Console run AR (2026-11-14) put a
  draw at about 10.5 us of the render thread's time, of which working out
  its programs and its fixed state (viewport, clip, cull, depth, stencil,
  bias) took about 4.
- **Fragment constants kept for the models.** A model's lit pixel shader
  reads its lighting from a volume texture, which the back end keeps as a
  2D texture with the depth in a uniform, and the rule above used to
  write a new fragment uniform buffer for every draw of such a program.
  Console run CP (2026-11-15) put that at about 4 us a model draw on the
  render thread, 215 draws at the busy view of `mp_crash`, with waits
  for the fragment ring on top: 33.4 ms a frame against 28.7 with the
  read left out. Now the buffer serves on while the volumes bound have
  the depths written into it, and a draw after a pixel constant changed
  keeps it when none of the constants its shader reads did (the
  translator lists them: `apiConstants` in its reflection). The picture
  is the same. The log's `fragment constants` lines count the buffers
  written and kept by each rule and time the reserve and the write on
  the timed draws. Empty files `no-frag-keep-depth` and
  `no-frag-keep-same` in the folder of the compiled programs, or
  `vita_gxm <name> 1`, turn the rules off one by one. `frag-direct-write`
  (off unless asked for) copies the constants into the buffer itself in
  place of `sceGxmSetUniformDataF`, for programs where a first copy
  matched sceGxm's own, to measure what that call costs.
- **The last draw's textures kept.** Going through a draw's textures
  was the largest part of a draw in the back end (about 2.1 us of 7.5 in
  console run CP), also when every one of them was set already. The
  D3D9 front now counts the changes to what a bind reads (a sampler's
  texture or state, a texture made anew, moved to other memory, left
  unfilled or destroyed), and a draw that finds the count, its pixel
  program, the mip bias and the frame as the last draw's textures were
  set with in the same scene sets none. An empty file `no-texture-keep`,
  or `vita_gxm no-texture-keep 1`, turns it off.
- **The last draw's state kept.** The D3D9 front counts every call that
  changes what a draw's programs and fixed state come from (a render
  state, a shader, the vertex declaration, a stream's stride, the
  viewport, the scissor rect, a target). A draw that finds that count, the
  open scene, its primitive type, its index width and its vertex stride as
  the last draw left them takes the last draw's programs and leaves the
  fixed state as it is, without the table lookup, the blend and the fixed
  state worked out again. Clears, copies and dropped programs end it. Most
  draws of one material (a run of static models or of world surfaces)
  change only their buffers and constants. An empty file `no-same-state`
  in the folder of the compiled programs, or `vita_gxm no-same-state 1`,
  turns it off.
- **No discard where the alpha test changes nothing.** The game's blended
  materials (decals, smoke, most effects) ask for the alpha test "greater
  than 0" beside their blend, which on a PC spares the blend of pixels
  that add nothing. The back end makes an alpha test a `discard` in the
  fragment program, and a tile GPU has to run such a program before it
  knows which pixels the primitive covers, which PowerVR's own advice is
  to avoid. Console run CD (2026-11-15) found
  the decals of one view of `mp_crash`, 122 draws of 1570, at 16.7 ms of
  the frame's 45.8 ms of fragment work. When the test rejects only alpha
  0, the blend adds nothing for alpha 0 and the draw writes neither depth
  nor stencil, the picture is the same without the test, so the draw
  takes the program without the discard
  (src/gfx_gxm/gxm_idle_alpha_test.h; `test_vita_idle_alpha_test.py`
  draws every such state both ways). The quit summary counts the draws
  (`alpha tests: ...`). An empty file `no-alpha-test-drop` in the folder
  of the compiled programs, or `vita_gxm no-alpha-test-drop 1`, turns it
  off. Console run CH (2026-11-15) measured 0.7 to 0.8 ms a frame at two
  views of `mp_crash` (34.54 against 35.36 ms, and 16.73 against
  17.45 ms).
- **Zero instead of a discard for the other blended draws.** A census of
  the same view on a PC (2026-11-15) found that most of the map's decals
  do not blend with the source alpha: 34 of their 50 materials carry
  premultiplied colour (blend One, InvSrcAlpha) with the alpha test
  "greater than 0", so a pixel of alpha 0 may still add colour and the
  rule above leaves their discard in. For a draw that blends, writes
  neither depth nor stencil and has a blend whose result for a source of
  (0, 0, 0, 0) is the picture as it was (destination factor One,
  InvSrcAlpha or InvSrcColor, operation add or reverse subtract), the
  fragment program writes (0, 0, 0, 0) for a pixel the test rejects
  where it would discard it (`alphaZero` in the translator's options,
  `AlphaTestCanZero` in gxm_idle_alpha_test.h). The picture is the same
  for any test and reference value, and the program has no discard. The
  quit summary counts these draws apart. An empty file
  `no-alpha-test-zero`, or `vita_gxm no-alpha-test-zero 1`, turns it
  off. What it saves on the console is not measured yet.
- **Depth first.** The scene's depth is always drawn before its lit pass
  on the Vita (src/gfx_gxm/gxm_engine_options.h), whatever r_depthPrepass
  says, so that the GPU skips hidden pixels of the lit pass early. Console
  run T (2026-11-11, mp_crash) measured the GPU's time a frame at 80 ms with
  it against 108 without at the busy view, and 27 against 50 facing the
  other way. The gun keeps its own narrow depth range; in the depth pass that
  range moves to a sliver just past it (1/64 to 1/64 + 1/65536), since the
  two passes' programs wrote depths a hair apart there and parts of the gun
  showed black. An empty file
  `no-depth-prepass` in the folder of the compiled programs turns the
  forced pass off. With the GPU at 222 MHz the pass costs the CPU about
  8 ms a frame (it draws every opaque surface twice), and console run AM
  (2026-11-14, mp_crash) found neither way wins everywhere: at the busy
  view at scale 0.7 a frame took 40.3 ms with it and 33.3 without, facing
  the other way 17.7 with it and 23.8 without. So the back end now times
  the two ways while the game runs (src/gfx_gxm/gxm_prepass_governor.h):
  it takes the median of 30 frames the way it is, tries the other way for
  30 frames when that way's time is old, and keeps a change only when it
  is at least 5% faster. The picture is the same both ways. The wait
  before a try starts at 8 seconds and doubles with each try that loses,
  up to 64; a time that moves by more than 15% (the view changed) brings
  it back to 8. Console run AO (2026-11-14, PR 327's first version, with
  window means and a try every 8 seconds) found its choices right but the
  tries cost 1 to 3 ms a frame on average (scale 0.7: 35.5 ms against
  32.7 with the pass held off), and one hitch dropped the pass on the
  light view for 9 seconds.
  Each change is logged (`diag: depth pass governor: off from frame ...`).
  An empty file `no-prepass-governor` keeps the pass on in every frame, as
  before.
  The forced pass draws the map's own surfaces and leaves the models out:
  walls and ground hide most of what a view hides, and console run AR found
  static models the largest part of the render thread's draws. The lit pass
  tests and writes depth itself, so the picture is the same. A pass that
  `r_depthPrepass 1` asks for, and the one that fills the depth copy for
  soft particles and depth of field, still draw everything. An empty file
  `no-world-only-prepass`, or `vita_gxm no-world-only-prepass 1`, puts the
  models back in the pass.
- **No distortion copy in frames that do not need it.** The Vita's GPU
  works a scene at a time: every change of render target ends one, stores
  its whole colour and depth targets and loads them again when the frame
  comes back. With r_distortion on the engine copied the picture for the
  surfaces that bend it (heat haze, glass) before the emissive pass of
  every frame, which split the back buffer's scene. The copy is now made
  only in frames whose emissive list holds a technique that reads it
  (src/gfx_gxm/gxm_engine_options.h). Console run AD (2026-11-14, mp_crash,
  busy view, 12 MB parameter buffer): the GPU took 79.4 ms a frame without
  the copy against 82.6 with it, 5 scenes a frame instead of 7. An empty
  file `no-resolve-skip` in the folder of the compiled programs turns it
  off. (Drawing the forced depth pass inside the scene's own targets, the
  other half of that work, made the GPU slower on the console: 90.8 against
  79.4 ms in the same run, so it was left out.)
- **The 3D picture at a lower resolution, the HUD at full.** The GPU's
  time grows with the pixels it shades (console run V: 95 ms a frame at the
  busy view, 77.7 with r_scaleViewport 0.7, 58.8 with 0.5). `vita_sceneScale`
  (0.5 to 1, 1 by default) draws the standard full-screen 3D view at that
  fraction of the width and height, in the top left of the screen, then
  copies it out and stretches it over the whole screen with linear
  filtering before the post effects and the HUD, which keep 960x544. The
  copy and the stretch are two short scenes (src/gfx_d3d/rb_draw3d.cpp,
  RB_StretchScaledScene; src/gfx_gxm/gxm_engine_options.h). Fullbright,
  the debug shaders and split views keep the full size. A game server
  cannot set it (server_dvar_guard.h). Console run AH (2026-11-14,
  mp_crash, busy view, David's settings): 63.1 ms a frame at 1, 57.8 at
  0.85, 49.8 at 0.7, 47.3 at 0.6 and 43.9 at 0.5, where the CPU's 40 ms
  becomes the limit. When the map's film colour filter is the view's only
  post effect, the filter's full-screen quad reads just the drawn corner of
  the copy it makes anyway, so the stretch costs no scene of its own; an
  empty file `no-filter-stretch` in the folder of the compiled programs, or
  `vita_gxm no-filter-stretch 1`, keeps the two scenes.
- **A 3D picture that shrinks only where the GPU needs it.** Console run
  BV (2026-11-15, mp_crash, busy view) found the frame held by the GPU's
  pixel work at the full size (35.5 ms a frame, 6.6 to 8.4 ms of it the CPU
  waiting for the frame before last) and by the CPU at 0.7 (29.6 ms, no
  wait). With `vita_sceneScaleMin` (0.5 to 1; 1 by default, which is off)
  under `vita_sceneScale`, the back end picks the size between the two
  frame by frame (`src/gfx_gxm/gxm_scale_governor.h`), in steps of 1/32 of
  the screen. Its measure is the time a frame waits for the GPU: the frame
  less its waits is what the CPU needs, and a frame with waits took what
  the GPU needs. Its goal is a frame every refresh (16.7 ms) where the CPU
  fills one in that time and every second refresh (33.3 ms) elsewhere,
  since with vsync a frame between the two is no smoother than 33.3 ms;
  `vita_sceneScaleGoal` names another goal in ms, and a goal the CPU does
  not reach means "the GPU must not hold the frame at all". Windows of 12
  frames, the 2 longest and 2 shortest left out: over the goal, the
  picture's area shrinks by the share that is over (times 1.4, as not all
  GPU time is pixel work); when a step more is known to fit, it grows a
  step; when the GPU's time is not known because it did not hold the
  frame, a step more is tried, and a try that fails is taken back and
  doubles the wait before the next (2 to 32 seconds). A goal of 16.7 ms
  that the smallest picture does not reach is given up for 16 seconds,
  doubled by each loss up to 128. The size costs nothing to change: the
  targets keep the screen's size and only the viewport moves. The log's
  `3D picture size` line, beside the frame time line, gives the sizes
  picked. While the depth pass governor tries the scene the other way the
  size stays as it is, so that the try is timed at one size; the size then
  follows the way that was kept. A game server can set neither dvar.
  Options > Game has the setting as "Dynamic Resolution" (Off, or down to
  85, 70 or 50 %) and `vita_sceneSharpen` as "Sharpen Scaled Picture"
  ([options-menu.md](options-menu.md)); both are saved in the player's
  settings.
  `scripts/tests/test_vita_scale_governor.py` runs it on a model of the
  console with run BV's times.
- **Keeping a smaller 3D picture sharp.** The Vita has none of the
  upscalers a PC pairs with a moving resolution (they need motion vectors
  and a history of frames), so the smaller picture is kept close to the
  full one in three plain ways, each with its own switch
  (`src/gfx_gxm/gxm_engine_options.h`):
  - *Mip bias, on by default.* A picture drawn smaller makes the GPU pick
    smaller mip levels, and the stretched picture would show blurrier
    textures than the full-size one. While the picture is scaled, every
    mipped texture's level of detail is biased by log2 of the scale, in
    sceGxm's steps of 1/8 of a level (one step at 0.92, four at 0.7), so it
    samples the texels the full-size picture would have. An empty file
    `no-mip-bias`, or `vita_gxm no-mip-bias 1`, turns it off. The start-up
    line `diag: a fresh texture's LOD bias reads N` gives the value the
    bias counts from.
  - *Width only* (`vita_sceneScaleWidthOnly 1`). The picture keeps its
    height and gives up the same share of its pixels in width alone: the
    width's factor is the scale squared (0.85 at a scale of 0.92), no less
    than a half. Every line of the screen is still drawn.
  - *A sharpening stretch* (`vita_sceneSharpen 1`). The stretch is drawn
    by a program of the back end's that reads the picture at each pixel
    and one texel up, left, right and down, adds 0.2 of the pixel's
    difference from its neighbours and holds the result between the
    darkest and the brightest of the five, so an edge gets steeper and
    gains no halo. As a stretch of its own it costs the copy's and the
    stretch's two scenes again. Where the film filter's quad does the
    stretch, that quad is drawn with a variant of the filter's own
    program whose reads of the picture go through the same filter
    (`gxm_cg_translate.h`, `Options::sharpSamplers`), and no scene is
    added; `no-filter-sharpen`, or `vita_gxm no-filter-sharpen 1`, goes
    back to the stretch of its own. Until the variant is compiled (once,
    then it is in the program cache) the quad is drawn plain.
  The HUD and all text are drawn after the stretch at 960x544 in every
  case. The `3D picture size` log line counts the biased texture views and
  the sharpened stretches. Console run CC (2026-11-15, mp_crash, busy
  view at 0.85): the mip bias costs nothing (33.05 ms a frame without,
  33.14 with), width only costs nothing (32.91), and the sharpening
  stretch of its own costs 2.3 to 3.1 ms a frame, which is all that 0.85
  saves there (35.82 ms at the full size). The sharpening in the filter's
  quad is not measured yet.
- **Three scenes a frame instead of five.** Each scene makes the GPU load
  the whole colour and depth targets and store them again, whatever is
  drawn: console run V measured 2.25 ms for a scene with no draw, and
  console run BX (2026-11-15) 16.9 ms of fragment work a frame in a view
  of 65 draws. The frame of a map with a film colour filter had five: a
  clear of the back buffer, the depth pass, the lit and emissive passes,
  the copy the filter reads, and the filter with the HUD. Three choices,
  each with the same picture (src/gfx_gxm/gxm_engine_options.h;
  src/gfx_d3d/rb_draw3d.cpp, RB_StandardDrawCommands):
  - the depth pass's scene clears the depth and stencil itself and the lit
    pass's scene clears the colour, so the scene that held only the clear
    is gone (`no-prepass-clear` turns it off);
  - the depth pass's scene is begun with sceGxm's disabled colour surface,
    since the pass writes no colour and nothing reads its colour target,
    so that scene loads and stores no colour (`no-depth-only-scene`). The
    engine marks the pass's draws for the back end, which keeps them in a
    scene of their own even when the lit pass draws to the same surface:
    depth and lit passes in one scene cost the GPU more (console runs AA
    and AD);
  - when the film filter is the view's only post effect, the 3D passes
    draw into the image the filter reads and not into the back buffer, so
    the copy between the two is not made (`no-scene-in-copy`). Depth of
    field, glow, blur and the debug views keep the back buffer and the
    copy, and so does a view that reads that image while it draws
    (distortion, shadow cookies).

  The three names are empty files in the folder of the compiled programs,
  or `vita_gxm <name> 1` while the game runs. The log's `gpu watch` line
  says how many scenes a frame had.
- **The CPU a frame ahead of the GPU.** The CPU fills frame N while the GPU
  still draws frame N - 1, so a frame costs the longer of the two times and
  not their sum. Console run U (2026-11-11, mp_crash) had shown them taking
  turns: every frame held one `sceGxmFinish`, 85 to 92 ms at the busy view
  beside 47 ms of CPU work, 135 ms a frame. The wait came from the game's
  DISCARD locks. The game discards its dynamic buffers every frame, the two
  skinned vertex caches and the two pretess index buffers in turn between
  frames, when the GPU has just been sent the frame that read them. The
  back end asked the main pool for fresh memory each time, found it full,
  and waited for the whole GPU instead. Three things replace that:
  - *Which frames are drawn* (`src/gfx_gxm/gxm_gpu_progress.h`). The last
    scene of every frame ends with a sceGxm notification: the GPU writes a
    value to a word of the notification region when that scene's fragment
    job ends. Every buffer and texture remembers the frame that last read
    it, and the CPU overwrites it only once that frame is drawn.
  - *Kept pieces of memory.* A discarded buffer the GPU still reads changes
    memory with a kept piece of its size and use that the GPU is done with;
    the first discard that finds none makes one. On mp_crash that is one
    more skinned cache (3 MB), dynamic index buffer (2 MB), pretess buffer
    and dynamic vertex buffer (1 MB each) and some small ones, which is why
    the main pool grew from 20 to 26 MB (and to 30 with the 16 MB parameter
    buffer, below). A texture the game rewrites whole
    every frame while the GPU samples it (the model lighting volume, 1 MB)
    gets a second piece the same way. Only when there is no piece and no
    room for one does a lock still wait for the whole GPU.
  - *The pacing wait.* After each Present the back end waits until the frame
    before last is drawn, and for that frame only. The CPU is then one
    frame ahead of the GPU and no more, which keeps the delay between the
    controls and the picture at what it was.
  The quit summary counts it: `buffer discards: N wrote in place (the GPU
  was done with the buffer), N took a kept piece of memory, N made a new
  one and N waited for the whole GPU (none free and no room); N texture
  rewrites took the second piece; kept pieces and second pieces hold N KB`
  and `frames overlap: yes ...; N of N frames waited for the frame before
  last, X ms each on average; N notifications did not come`. With the frame
  time line the log names the cause of every wait for the whole GPU since
  the last one: `waits for the whole GPU since the last such line: N x a
  buffer lock without a copy to write, ...`; no such line means none.
  If a notification does not come within 2 s three times, the back end
  says so and goes back to one wait a frame. An empty file
  `no-frame-overlap` in the folder of the compiled programs asks for that
  from the start, so that one console sitting can compare the two. An empty
  file `no-vsync` there shows each frame without waiting for the display's
  refresh (it tears): for measuring what that wait costs. With two frames
  on their way, sceGxm's parameter buffer and the context's ring buffers
  hold the geometry, draw calls and uniform buffers of both at times.
  The parameter buffer, which comes out of CDRAM, is 16 MB, sceGxm's own
  default. The GPU's vertex job for one frame runs beside the fragment job
  of the frame before, and the buffer holds the binned geometry of both.
  When both do not fit, the vertex job waits until the frame before is
  drawn and the two jobs take turns. Console run AD found 8 MB too small
  at every view (12 MB was 10 to 19 ms a frame faster, 14 to 18%, in every
  phase). Console run CS (2026-11-15, full 3D size) found 12 MB too small
  at mp_crash's heaviest view (`setviewpos -256 2272 291 270 0`): the frame
  there flipped by itself between 38 ms and 49 to 51 ms, the GPU's vertex
  job between 22 and 48 ms, and a saving on the CPU tipped it to the slow
  side, so that a faster render thread measured as a slower frame. With
  16 MB every window stayed at 37 to 38 ms; with 8 MB every window took 49
  to 52. The 4 MB come out of the CDRAM pool, which on mp_crash then peaks
  at 92.9 of 96 MB with about 1 MB gone over to main memory, so the main
  pool grew from 26 to 30 MB and the client's heap went from 64 to 60 MB
  (mp_crash uses 34). For comparing, an empty file in the folder of the
  compiled programs gives another size: `small-parameter-buffer` 8 MB,
  `mid-parameter-buffer` the former 12 MB, `big-parameter-buffer` 20 MB.
  At this view a timing is only worth reading with the `gpu watch` line
  beside it: a vertex job of 40 ms or more means the two jobs took turns.
  sceGxm's ring buffers have its default sizes: 128 KB for draw calls,
  2 MB for vertex constants and 512 KB for fragment constants. A draw that
  finds a ring full waits inside sceGxm until the GPU has finished a
  scene, and no line of the log names that wait. Console run BV
  (2026-11-15, the busy view of `mp_crash`, 1500 draws a frame) found it:
  about one timed draw a frame took 5 to 11 ms, in `sceGxmDraw` or where
  the fragment constants are reserved, and with the rings doubled none
  did. PR 376 doubled the draw calls' and the fragment constants' rings
  for that; console run BY measured about 1 ms a frame at that view. But
  the wait is also what keeps the CPU from handing the GPU more than it
  can take. Console run CJ (the same day, three starts of one build, a
  heavier view of 1570 draws at full size) measured 40.8, 44.1 and 41.9 ms
  a frame with the default rings against 56.7, 50.4 and 62.1 ms with the
  two doubled (and 59.1, 49.2, 49.5 with all three doubled): the GPU's
  vertex work took 22 to 26 ms against 38 to 61, with the same fragment
  work (36 to 41 ms) and the same CPU time on every thread. The busy view
  was 35.0 ms against 34.2. So the defaults are back. `double-gxm-rings`
  gives the two doubled rings (and the main pool 1 MB more),
  `big-gxm-rings` all three doubled (3 MB more), for comparing.
- **What the CPU pays for GPU memory.** The main pool is memory the CPU
  does not cache (`SCE_KERNEL_MEMBLOCK_TYPE_USER_RW_UNCACHE`), as Sony's
  samples have it, and CDRAM is not cached either. The engine skins
  vertices into the pool and reads some of them back, writes a frame's
  indices there and fills the uniform buffers there, on three threads. At
  start-up the back end measures what that memory costs and logs one line:
  `gpu memory speed, 256 KB at a time: the heap: copy in N MB/s, copy out
  N MB/s, a word written N ns, read N ns; main: ...; cdram: ...` (the
  shortest of four passes each; "copy out" and "read" are the CPU reading
  the pool). An empty file `cached-main-memory` in the folder of the
  compiled programs gives the main pool as ordinary cached memory
  (`diag: cached-main-memory is there ...`; the two memory lines then say
  `main (cached)`), so that one console sitting can say what the uncached
  kind costs a frame. It is for measuring: whether the GPU always sees what
  the CPU wrote last from cached memory is not known, so the picture may
  be wrong with it.
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
- **Skinning.** Animated models (the hands and weapon, players) are skinned
  on a worker thread into the skinned cache. Console run BD measured 6 ms a
  frame for the 3227 vertices of the hands and weapon alone, 1.9 us a
  vertex, with the engine's scalar code: it writes a vertex field by field
  into memory the CPU does not cache and reads a weighted vertex's position
  back from it, on a CPU whose NEON the build did not use. The client now
  skins with the vector path (`src/gfx_d3d/r_model_skin_vec.cpp`, the one
  file built for NEON; [the rule](linux-armv7.md)): the same bytes, a vertex
  put together in registers and written once. `r_sse_skinning 0` goes back
  to the scalar code, so `perf_ab "set r_sse_skinning 0" "set r_sse_skinning 1"`
  holds the two against each other (`other threads run: skin xmodel` is its timer).
  Not run on a console or in Vita3K yet.

### Shaders (step 2c)

`src/gfx_gxm/shader/gxm_cg_translate.cpp` turns D3D9 shader bytecode
(vs_1_1 to vs_3_0, ps_1_1 to ps_3_0) into Cg: a float4 local per register and
one statement per instruction. Loops, calls, predicates, `if` on boolean
constants, `vPos`, `vFace`, more than one colour output and ps_1_4 are
refused with a reason; the game's shaders use none of them (the renderer
plan's corpus scan). ps_1_1 to ps_1_3, which the game's shader model 2 set
(`r_rendererPreference 0`) draws its depth pass with, are rewritten as
ps_2_0 when parsed: `tex t0` reads sampler 0 at coordinate 0, the colour is
what r0 holds at the end; their samplers are taken to be 2D, and the
instructions that address one texture through another are refused. GXM has no volume textures, so a volume is stored as its
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
count what it held and gave back.

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

**Static vertices and indices are read in place.** A fast file's zone keeps
its own copy of the vertex and index blocks and of a map's world vertices
for as long as it is loaded; on a PC the renderer copies them into vertex
and index buffers. On the Vita those copies took 25 to 45 MB of a map's
96 MB of CDRAM (mp_carentan's world vertices alone are 22 MB, and no
single free range was that long). So the zone arena's memory block is given
to the GPU (`sceGxmMapMemory`, read only) and a static buffer made from a
zone's data points at that data (`CreateVertexBufferOver`, reached through
D3D9's shared handle of `CreateVertexBuffer`). Its Unlock cleans the range
out of the CPU's caches, with DMA copies into a small scratch block (user
code cannot clean caches; the kernel cleans a DMA copy's source), and
destroying it waits for the GPU if a draw read it, so the zone may reuse
the memory at once. The `gpu memory by use` line counts them as `static
vertices in place` and `static indices in place`. A file named
`no-buffers-in-place` beside the compiled programs gives them GPU memory of
their own, as before.

The Legacy back end counts each pool's use by what it is for and logs it
with the engine's other figures (once a minute by default, `vita_statsEvery`,
and before a timed quit) and at every failed allocation (`gpu memory by use (cdram +
main MB, pieces): ...`), and logs each piece of 1 MB or more as it is made
(`gpu memory piece N: ...`, with its size, format and usage).

With the same figures, and once more when the device goes, it says where a
frame's time went since the last such line, as a frame's mean:
`gxm legacy: frame time, last N frames: X ms a frame. In this back end: N
draws X ms (one in 16 timed), N clears and copies X ms, N scenes' starts and
ends X ms, N waits for the GPU X ms (longest X ms), N waits for the frame
before last X ms (longest X ms), showing the frame X ms (longest X ms). The
rest, X ms, is the game, the D3D9 front and the filling of buffers and
textures. ...`. How to read it:
- **Waits for the GPU** are `sceGxmFinish` calls: a discarded buffer with no
  piece of memory to write, a target read back. Each stops the CPU until the
  GPU has drawn everything sent so far, so one in every frame makes the CPU
  and the GPU take turns. The line after it names their causes.
- **Waits for the frame before last** are the pacing wait. The GPU draws the
  last frame meanwhile, so a large figure here means the GPU holds the frame
  and the CPU has time to spare; a figure near zero means the CPU does.
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
- **A draw's parts** end the line once some draws were timed step by step:
  about one in 32 of the draws not timed whole reads the clock after each
  step (setup and finding the programs, the fixed state, setting the
  programs and the vertex constants, the fragment constants, binding the
  textures, the streams and `sceGxmDraw`), and the line gives each step's
  mean in microseconds with one clock read taken off. Those draws are left
  out of the draws' sum, and one with scene work or a wait inside, or one
  that ended early, is dropped.
- **The rest** is everything outside those calls. The system's `Vita load`
  line beside it says how busy the processor was.

The sums are plain arithmetic (`gxm_frame_time.h`), tested on the host by
`scripts/tests/test_vita_frame_time.py`. In Vita3K its figures are the
emulator's.

**What the frame time line cannot say** is how long the GPU works. A CPU
that waits for the GPU does not always do it in a call the line names: in
console runs BO, BP and BQ (2026-11-15, the busy view of `mp_crash` at
37 ms a frame) there was no wait for the GPU and almost no pacing wait,
yet about one timed draw a frame took 5 to 11 ms, in the step that reserves
room for the draw's fragment constants (BQ) or in the one that hands the
draw to sceGxm (BP), and every millisecond saved on the render thread made
that one draw a millisecond longer. sceGxm keeps a frame's draws and their
constants in ring buffers that the GPU frees as it finishes scenes, so a
draw that finds a ring full would wait there for the GPU.

The **GPU watch** measures the GPU itself. With an empty file `gpu-watch` in
the folder of the compiled programs, or `vita_gxm gpu-watch 1` while the game
runs, every scene carries two sceGxm notifications, one for the end of its
vertex job and one for the end of its fragment job, and a thread of the back
end's (`gpu watch` in the thread table, above the game's threads in
priority) looks at the notification region every quarter of a millisecond
and notes when each came. Beside the frame time line the log then says:
`gxm legacy: gpu watch, last N frames (X ms a frame): the GPU's vertex work
X ms a frame, its fragment work X ms a frame, in N scenes (longest for one
scene X and X ms); it finished a frame X ms after the frame's last scene was
sent (longest X ms); N scenes not watched`. How to read it:
- **Vertex work** of a scene runs from when the scene was sent, or the
  vertex job before it ended if that was later, to the end of its vertex
  job: the GPU running the vertex programs and sorting triangles into
  tiles. It grows with vertices and draws, not with the size of the picture.
- **Fragment work** runs from the end of the scene's vertex job, or of the
  fragment job before it, to the end of its fragment job: the GPU shading
  pixels. It grows with the size of the picture and the cost of the pixel
  programs.
- The two are done by different parts of the GPU and overlap, so their sum
  is not the GPU's frame. When either is about as long as the frame, the
  GPU sets the frame's length and that half of it is where to look; when
  both are well under it, the CPU does.
- **Finished a frame N ms after** its last scene was sent is how far behind
  the CPU the GPU runs. Near a whole frame, the CPU is waiting for it
  somewhere.

The watch is off by default: its thread wakes four thousand times a second.
Its sums are plain arithmetic (`gxm_gpu_watch.h`), tested on the host by
`scripts/tests/test_vita_gpu_watch.py`. Not run on a console or in Vita3K
yet.

The display's own buffers take about 9 MB of CDRAM and 2.6 MB of main memory.
The parameter buffer, which sceGxm takes itself, adds 16 MB (out of CDRAM:
in console run CS the game's CDRAM pool came out at 112 MB less the buffer).
The probe measures what is left.

## Stages

The plan, with reasons and risks: `vita-client-plan.md` in the project files.

1. **Build and boot** (this page).
2. **Screen:** a GXM back end for the Legacy adapter API (`src/gfx_vk/legacy/vk_legacy.h`), under the existing D3D9 front (`src/gfx_vk/d3d9`). Game shaders are translated to Cg, compiled on the Vita by `libshacccg.suprx` (each user extracts it from their own firmware) and cached as `.gxp`.
3. **Controls and sound:** buttons, sticks and touch become engine input, and sound gets a Vita output. Built and tested on the host ("Controls", "Sound"). Text is typed on the system's on-screen keyboard ("Typing"): it opens and closes in Vita3K, and a text given by a switch reaches the game there (runs KB1 and KB2); it has not run on a console.
4. **Join and play:** connect to a server and play, with the visual cuts from the feasibility study.
5. **Make it fast:** profile on the Vita, then cut the hot spots.
