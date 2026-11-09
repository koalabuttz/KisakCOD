# PS Vita multiplayer client

`-DKISAK_BUILD_VITA_MP_CLIENT=ON` (with `KISAK_PLATFORM=vita`) builds the
multiplayer client, `KisakCOD-mp`, for a hacked PS Vita and packs it as
`KisakCOD-mp.vpk` (title ID `KSKD00002`). It is opt-in and off by default, and
it follows the [Vita dedicated server](vita.md), which shares its toolchain,
platform layer and memory rules. Started 2026-11-09.

**State: stage 1 done (build and boot); stage 2 (graphics) under way: the
GXM probe ran at 60 fps, the device ran the menu loop without drawing, and
the game's shaders compile on the Vita (466 programs, none failed; read back
from the card in 1.4 s). Step 2d, the first real draws, is built and not run
on the Vita yet.**
- The whole client compiles, links and packs into a `.vpk`: client, cgame, UI, sound, renderer and its own listen server.
- Stage 1 had no graphics back end, so the client stopped at the renderer through `Sys_Error`. Stage 2 replaces that: the renderer gets the D3D9 front on the GXM back end ("Graphics" below), which does everything but draw.
- Sound links the OpenAL stand-in ("no device").

**Runs on David's Vita (2026-11-09):**
- Run 1 reached the renderer, but its log was empty. A game client's `Sys_Print` was silent: on Linux `Com_PrintMessage` copies lines to stderr, which the Vita lacks. It then aborted on an assert, because `R_SetWndParms` read an empty display-mode list after the "no device" answer. Both are fixed.
- Run 2 stopped where expected: the banner at 1.6 s, `FS_Startup` at 2.0 s, the network up at 8.8 s, then `R_Init` and the fatal error at 9.0 s. No zones load before the renderer starts.
- At the error: zone arena 128 MB free, heap 5.8 of 64 MB used, engine reservations 154 MB, and **61 MB of system memory free** (220 MB free at start-up).
- Run 2 also showed every `.iwd` opened twice: newlib's `getcwd` spells the folder `ux0:/data/KisakCOD`. `Sys_Cwd` now drops that slash.

## Building

As for the server (see [vita.md](vita.md)), plus two header directories:
- **DXVK Native's headers** supply the D3D9 declarations: the types and interface layouts only. No DXVK code is built or linked. The pinned 2.7.1 headers come from `scripts/tests/run_native_d3d9_boundary_checks.py --stage-dxvk-headers`, as in the Linux client jobs.
- **OpenAL's `AL/al.h`** comes from libopenal-dev or the OpenAL Soft source.

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
uploads the RelWithDebInfo `.vpk`.

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
   creates is translated to Cg and compiled (`gxm legacy: shaders so far: ...`
   every 100 shaders, `shaders in all` at the end), and the compiled programs
   are kept in `ux0:data/KisakCOD/gxp`. `gxm legacy: draws: ...` lines at the
   first frame, after ten seconds and every minute count what was drawn and
   what was skipped, and why. The client needs
   `ur0:data/libshacccg.suprx`, as the GXM probe does; without it the log
   says so and nothing is drawn.

START + SELECT quits.

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
| Window procedure, input | `src/vita/client/vita_client_input.cpp` | Sets `g_wv.hWnd`; buttons are not mapped yet |
| Graphics device | `src/vita/client/vita_d3d9.cpp` | The D3D9 front's host: one 960 x 544 mode, one monitor, the log |
| D3D9 front | `src/gfx_vk/d3d9` | The Vulkan work's IDirect3D9, unchanged, over the seam in `vk_d3d9_backend.h` |
| GXM back end | `src/gfx_gxm` | The Legacy API on sceGxm (`legacy/gxm_legacy.cpp`), the screen, GPU memory |
| Sound | `tests/linux_mp_client/openal_unavailable.cpp` | OpenAL stand-in until the Vita has an OpenAL |
| Linux client pieces | `msvc_rand.cpp`, `mp_client_unavailable.cpp`, `texture_memory.cpp`, the Material_Sort span | As on Linux |

`deps/binklib/radbase.h` gains a `__vita__` branch (the Linux one, with the
Vita's CPU), because the renderer's headers declare the Bink API.

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

The heap and the engine reservations are the server's for now: a 64 MB heap, the 128 MB zone arena and the hunks. Stage 1's run measures what is left. Textures will go to the GPU's own memory (CDRAM, about 112 MB), not main memory.

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
- Locks of the back buffer, cube and depth textures as shader inputs,
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
  destination with two more built-in programs.
- **Clears** are quads drawn in the scene with two small built-in programs:
  colour by the fragment program, depth and stencil by an always-pass depth
  test. sceGxm has no clear call, and fills on the transfer queue would race
  the scenes.
- **Programs.** Each draw picks its shaders' variants: the vertex
  declaration decides which inputs are D3DCOLOR (swapped), missing (read as
  0, 0, 0, 1) or narrower than four components, and the alpha test picks the
  pixel shader's. A variant other than the default compiles on first use
  (through the cache) and is registered with the shader patcher. Vertex
  programs are kept per variant, declaration and stream strides; fragment
  programs per variant, vertex program, blend state and target format.
- **Textures** are linear: rows of whole multiples of 8 texels, and the
  levels of a mipmapped texture sized from the power of two above the base,
  which is how sceGxm reads them and also a layout LockRect can hand out.
  Render-target textures are strided (rows of 32 texels, as colour surfaces
  want). Volume textures are their slices stacked. Cube and depth textures
  are not sampled yet: draws bind a grey cube or a white texture.
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
console compiles a program once (about 0.1 s each). A program that does not
compile is logged with the compiler's first error (the first 40 failures;
each reason a shader is not translated is logged once and counted at the end)
and its Cg is left beside the cache as `failed-<hash>.cg`. A shader that
cannot be translated or compiled is still created, as D3D9 would; draws with
it are skipped and counted.

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

The display's own buffers take about 9 MB of CDRAM and 2.6 MB of main memory.
The parameter buffer, which sceGxm takes from main memory itself, adds 16 MB.
The probe measures what is left.

## Stages

The plan, with reasons and risks: `vita-client-plan.md` in the project files.

1. **Build and boot** (this page).
2. **Screen:** a GXM back end for the Legacy adapter API (`src/gfx_vk/legacy/vk_legacy.h`), under the existing D3D9 front (`src/gfx_vk/d3d9`). Game shaders are translated to Cg, compiled on the Vita by `libshacccg.suprx` (each user extracts it from their own firmware) and cached as `.gxp`.
3. **Controls and sound:** buttons, sticks and touch become engine input, and sound gets a Vita output.
4. **Join and play:** connect to a server and play, with the visual cuts from the feasibility study.
5. **Make it fast:** profile on the Vita, then cut the hot spots.
