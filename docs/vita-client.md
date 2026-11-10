# PS Vita multiplayer client

`-DKISAK_BUILD_VITA_MP_CLIENT=ON` (with `KISAK_PLATFORM=vita`) builds the
multiplayer client, `KisakCOD-mp`, for a hacked PS Vita and packs it as
`KisakCOD-mp.vpk` (title ID `KSKD00002`). It is opt-in and off by default, and
it follows the [Vita dedicated server](vita.md), which shares its toolchain,
platform layer and memory rules. Started 2026-11-09.

**State: stages 1 to 3 run on the console. On 2026-11-10 the main menu
showed on David's Vita and worked, with touch, buttons and music (run K).
Starting a map does not work yet: the GPU's memory is full before the map
loads ("Memory" below), which is the next piece of work.**
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

Hosted CI: the Linux workflow's **PS Vita multiplayer client** job builds Debug
and RelWithDebInfo. It runs the server's float checks (no fused multiply-add, no
NEON float) and link checks (weak pthread references, the main stack size), and
uploads the RelWithDebInfo `.vpk`. It also runs the controls' and the sound's
host tests (`scripts/tests/test_vita_input.py`, `test_vita_sound.py`).

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
dedicated server takes it too).

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
the read itself counted no lit pixel.

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
- `cls` 2.9 MB, `odeGlob` 2.8 MB, `gfxBuf` 2.3 MB, `s_backEndData` 2.2 MB, `tess` 2.2 MB;
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
- Still to do: the dynamic buffers are sized for a PC (the static model cache 8 MB, the skinned caches 9 MB, the index buffers 10 MB).

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

**On the console.** Run 1 (2026-11-09, in the menus, with nothing drawn yet
and `vita_inputDebug 1`): every button arrived as the key in the tables
below, the right stick as the wheel, and a start-up without a profile got the
default layout and the profile name. Run K (2026-11-10, with the menu on the
screen): the screen works as the menu's pointer. The menu was worked by touch
up to starting a server, and the log has nine touches, each a `MOUSE1` press
and release within about 20 ms. Touches in the top-left, top-right and
bottom-left corners of the panel landed in the same corners of the engine's
960 by 544 screen. Not confirmed yet: the
screen's three areas in a game, which side of the rear pad is which, and the
look speed and the layout in a game, which need a game on the screen.

### In the game

Every control is a key of its own: AUX1 to AUX16 and the keypad arrows, which
no PC keyboard or mouse sends. The default layout is the console game's where
the Vita has the button.

| Control | Engine key | Default binding | Does |
|---|---|---|---|
| Left stick | `AUX9` up, `AUX10` down, `AUX11` left, `AUX12` right | `+forward`, `+back`, `+moveleft`, `+moveright` | Move |
| Right stick | mouse motion | | Look |
| R | `AUX6` | `+attack` | Fire |
| L | `AUX5` | `+speed_throw` | Aim down the sight |
| Cross | `AUX1` | `+gostand` | Jump, stand up |
| Circle | `AUX2` | `+stance` | Crouch; hold for prone |
| Square | `AUX3` | `+usereload` | Use, reload |
| Triangle | `AUX4` | `weapnext` | Next weapon |
| D-pad up, down, left, right | `KP_UPARROW`, `KP_DOWNARROW`, `KP_LEFTARROW`, `KP_RIGHTARROW` | `+actionslot 1` to `4` | The console game's d-pad |
| Select | `AUX7` | `+scores` | Scoreboard |
| Start | `ESCAPE` | fixed | Menu |
| Rear touch pad, a tap on the left or right half | `AUX8`, `AUX16` | `+breath_sprint` | Sprint (the left stick's click on a console) |
| Screen, left third | `AUX13` | `+smoke` | Special grenade |
| Screen, right third, top half | `AUX14` | `+frag` | Frag grenade |
| Screen, right third, bottom half | `AUX15` | `+melee` | Knife |

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
  lifts, wherever it slides. The middle third of the screen is no key.
- **The rear pad** is a tap. The fingers that hold the console lie on it: the
  first console run logged 30 touches in 40 seconds where two taps were asked
  for. So a finger that stays longer than 0.3 s is resting and does nothing. A
  tap presses its half's key when the finger leaves, and the key stays down
  while the left stick is pushed forward and for a quarter of a second after.
  With the default binding that is the console game's sprint: tap, and run
  until you stop. A tap while standing still is a short press, and a tap while
  the key is down presses it again. Holding your breath behind a scope wants a
  held key, so give `+holdbreath` a control of its own.

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
game key, and Start cancels. The menu and the game's hints show the engine's
names (`AUX6`, not "R"); the table above translates.

A profile from a PC, a new profile and the menu's "reset to default" all
leave the AUX keys bound to nothing. While none of AUX1 to AUX16 has a
binding, the default layout is set (`vita_defaultBinds`, on by default; the
log says so). One control unbound on purpose stays unbound. `vita_binddefaults`
sets the whole default layout at any time.

| Dvar | Default | |
|---|---|---|
| `vita_stickDeadzone` | 0.2 | Part of a stick's travel around the centre that does nothing |
| `vita_lookSpeed` | 1400 | Right stick at full deflection, in mouse counts a second |
| `vita_lookPitchRatio` | 0.65 | Up and down, as a part of `vita_lookSpeed` |
| `vita_lookCurve` | 2 | 1 is linear; higher is finer near the centre |
| `vita_cursorSpeed` | 700 | Left stick in a menu, pixels a second |
| `vita_frontTouch` | 1 | The screen's three areas are keys in the game |
| `vita_rearTouch` | 1 | A tap on either half of the rear pad is a key in the game |
| `vita_defaultBinds` | 1 | Set the default layout while no AUX key is bound |
| `vita_inputDebug` | 0 | Log every key sent, every finger that lands (with its panel and screen position) and how long it stayed |

### Not done yet

- **The on-screen keyboard** for text fields (names, server addresses, chat).
  It needs a hook in the frame's present, so it follows the first drawn
  frames. Until then one field is filled in: with no player profile, the
  "create profile" box is offered the name `Vita` to accept.
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

Not measured yet: all of this on a console (Vita3K's memory figures are not a
console's), what the encoding adds to a load there, the claim count and which
sounds claim too much, the path that keeps a sound's own IMA ADPCM blocks with
real data, and how the encoded sounds sound. Nobody has listened to them: the
runs had no listener.

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
frames per update` when sound starts, then after ten seconds, once a minute
and once more when sound stops `vita sound: ... updates, mixing ... us each on
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
  the scenes.
- **Programs.** Each draw picks its shaders' variants: the vertex
  declaration decides which inputs are D3DCOLOR (swapped), missing (read as
  0, 0, 0, 1) or narrower than four components, and the alpha test picks the
  pixel shader's. A variant other than the default is asked for on first
  use and made on the cache's thread; the draw is left out until it is
  ready, then it is registered with the shader patcher. Vertex
  programs are kept per variant, declaration and stream strides; fragment
  programs per variant, vertex program, blend state and target format.
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
  two frames ahead. DrawPrimitiveUP and the clear quads copy their vertices
  into a 512 KB per-frame ring.

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
once a minute and at every failed allocation (`gpu memory by use (cdram +
main MB, pieces): ...`), and logs each piece of 1 MB or more as it is made
(`gpu memory piece N: ...`, with its size, format and usage).

The display's own buffers take about 9 MB of CDRAM and 2.6 MB of main memory.
The parameter buffer, which sceGxm takes from main memory itself, adds 16 MB.
The probe measures what is left.

## Stages

The plan, with reasons and risks: `vita-client-plan.md` in the project files.

1. **Build and boot** (this page).
2. **Screen:** a GXM back end for the Legacy adapter API (`src/gfx_vk/legacy/vk_legacy.h`), under the existing D3D9 front (`src/gfx_vk/d3d9`). Game shaders are translated to Cg, compiled on the Vita by `libshacccg.suprx` (each user extracts it from their own firmware) and cached as `.gxp`.
3. **Controls and sound:** buttons, sticks and touch become engine input, and sound gets a Vita output. Built and tested on the host ("Controls", "Sound"); the on-screen keyboard follows the first drawn frames.
4. **Join and play:** connect to a server and play, with the visual cuts from the feasibility study.
5. **Make it fast:** profile on the Vita, then cut the hot spots.
