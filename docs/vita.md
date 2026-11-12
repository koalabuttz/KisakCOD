# PS Vita dedicated server

`KISAK_PLATFORM=vita` builds the multiplayer dedicated server, `KisakCOD-dedi`,
for a hacked PS Vita (HENkaku/Ensō), packed as `KisakCOD-dedi.vpk`. It is an
opt-in track started 2026-11-07, after the [armv7 Linux build](linux-armv7.md)
proved the engine on the Vita's CPU family. Nothing here changes the Linux,
macOS or Windows builds.

**State:** the server compiles, links and packs into a `.vpk` (sandbox and
hosted CI), and boots in the Vita3K emulator: start-up, the data folder, the
log, threads and the file system work, and with no game data it stops at the
missing first zone, as the Linux build does. The first run on David's Vita
(2026-11-08) started up, logged 208 MB of main memory free, then crashed
while listing the `.iwd` files: an 8-byte store to an odd address, which the
Vita's CPU refuses and Vita3K allows (see "Alignment" below). The main
thread also had 256 KB of stack instead of 8 MB. The second run loaded
mp_crash's zones in 32 s and crashed as the game started: every `thread_local`
was one copy shared by all threads (see "Thread-local storage" below). The
third run started the game and stopped in the first script vector sum: the
script's vector constants sit unaligned in the bytecode, and float loads
(`VLDR`) fault there (see "Alignment"). All are fixed; the network, the map
and the memory budget are next checked on the console with the steps below.

The Vita build takes at most **24 players** (`sv_maxclients` 1 to 24, default
24). The server's snapshot buffers are sized by that cap; at retail's 64 they
alone would not fit in the Vita's memory (see "Memory" below).

## Building

Needs Clang (the engine's Microsoft C++ extensions rule out GCC), CMake, Ninja
and VitaSDK. `scripts/ci/install-vitasdk.sh` downloads the pinned VitaSDK
snapshot from the vitasdk/autobuilds releases, checks its SHA-256 and prints
the SDK root:

```sh
export VITASDK="$(scripts/ci/install-vitasdk.sh "$HOME/vitasdk-pinned")"
cmake -S . -B build-vita -G Ninja -DKISAK_PLATFORM=vita -DCMAKE_BUILD_TYPE=RelWithDebInfo
cmake --build build-vita --target KisakCOD-dedi-vpk
# -> bin/KisakCOD-dedi (ELF with symbols) and bin/KisakCOD-dedi.vpk
```

Hosted CI: the **PS Vita dedicated server** job of the PS Vita workflow
(`.github/workflows/build-kisakcod-vita.yaml`, [what runs when](ci.md)) builds Debug
and RelWithDebInfo, checks the binary for fused multiply-add and NEON float
instructions, and uploads the RelWithDebInfo `.vpk` as an artifact.

## Running on the Vita

1. Install `KisakCOD-dedi.vpk` with VitaShell. The bubble has no icon yet.
2. Copy the PC game's `main/` and `zone/` folders to `ux0:data/KisakCOD/`
   (for mp_crash: `zone/english/code_post_gfx_mp.ff`, `common_mp.ff`,
   `localized_common_mp.ff`, `mp_crash.ff`, `localized_mp_crash.ff`, plus
   `main/*.iwd`). Game data stays on your devices.
3. Write the server's command line to `ux0:data/KisakCOD/cmdline.txt`, all
   on one line. Without it the server runs `+set dedicated 1 +map mp_crash`.
   Quotes keep spaces in one argument. The Vita has no Steam, so clients join
   with a key code (a KisakCOD client in key-code mode, or a retail 1.7
   client), which the server refuses unless `sv_allowUnverifiedGuids` is on:
   `+set dedicated 1 +set sv_allowUnverifiedGuids 1 +map mp_crash`. That
   accepts unverified GUIDs, so keep it to your own network. The
   [Linux guide](linux-dedicated.md) lists the other settings (a retail 1.7
   client also needs `sv_allowProtocol6 1`).
4. Turn Wi-Fi on and start the bubble. The screen stays black: the server has
   no display. Everything it prints goes to `ux0:data/KisakCOD/console.log`.
5. Join from a PC on the same network (`/connect <vita ip>`). The Vita's
   address is in the log (`IP: ...`).
6. Hold **START + SELECT** to quit cleanly (the same as typing `quit`).

VitaShell's FTP server reaches `ux0:data/KisakCOD` from a PC for new builds,
`cmdline.txt`, `console.log` and crash dumps (`ux0:data/*.psp2dmp`). It serves
only while its FTP screen is open, so quit the server first.

Console runs so far (2026-11-08/09): runs 1-3 each crashed on one fault (an
unaligned 8-byte store, shared thread-locals, an unaligned script vector),
all fixed. The run 4 build ran 9 h 10 min idle on mp_crash without a crash
(8 % busy, memory flat: 46 MB of the zone arena and 45 MB of system memory
free), and a key-code client on the laptop joined it over Wi-Fi (ping 18 ms;
15-17 % busy with one player, slowest frame about 20 ms after the join
minute).

What to send back from the first runs: `console.log` and whether a client
could join. The log answers the rest (next section). Then run with 0, 4 and 8
bots for a few minutes each and send that log too (bots: see
[Server-side test clients](server-test-clients.md)).

### Reading console.log

Each line starts with the seconds since launch, so the gap between
`Loading fastfile mp_crash` and the first frames is the map load time. Lines
starting `Vita` come from the Vita layer:

- `free memory at start-up`: what the system had left after loading the
  program and newlib's heap, before the engine's reservations.
- `Vita memory ...` (after start-up, then once a minute, before a timed
  quit, and at a fatal error): game memory free in the zone arena, heap use out of its block, the
  engine's reservations and what the system still has free. An out-of-memory
  error prints it last, which shows the pool that ran out.
- `Vita load, last 60 s: busy 12.3%, slowest frame 18.2 ms, 0 frames over
  50 ms`: the share of time the main thread worked rather than waited for
  packets, its longest frame, and how many frames took longer than one server
  frame at `sv_fps 20`. The minute with the map load shows the load as its
  slowest frame. Busy near 100% or frames over 50 ms in play mean the Vita is
  not keeping up with that player count.

The `vitastats` command (console or rcon) prints both lines on demand, and
has the client's renderer and sound add theirs; the engine's `meminfo` lists
the zone arena by zone.

### Runs nobody watches

A start in an emulator, or on a console with nobody at the buttons, should
end by itself as soon as it has its numbers. These dvars go in the command
line file (`cmdline.txt`, `cmdline-mp.txt` for the client). The logic is
`src/vita/vita_run_plan.h`, checked on the host by
`scripts/tests/test_vita_run_plan.py`.

The Vita's C library does not know the `%zu`, `%jd` and `%td` printf
conversions: it prints the letters and reads the later arguments as the wrong
types. Cast and use `%u` or `%llu`; the same test reads `src/vita` and
`src/gfx_gxm` for them.

| Dvar | What it does |
| --- | --- |
| `vita_quitAfterMenu <s>` | Quit this many seconds after the main menu first showed (client). |
| `vita_quitAfterMap <s>` | Quit this many seconds after the map was first in play: the client is in the game, or the dedicated server runs a map. |
| `vita_quitAfter <s>` | Quit this many seconds after start-up. Set it as well, as the safety net for a start whose menu or map never comes. |
| `vita_statsEvery <s>` | The period of the load, memory, GPU memory and sound lines (60 by default). The lines also come the moment the menu or the map is up, and the period counts from there. |
| `vita_atMenu "<s> <command> \| <s> <command>"` | Console commands to send that many seconds after the menu first showed. Read at start-up only. |
| `vita_atMap "..."` | The same after the map was first in play. `{serverid}` in a command stands for the server id. Read at start-up only. |
| `vita_atMenu "@<file>"`, `vita_atMap "@<file>"` | The same steps from a file in the data folder (`ux0:data/KisakCOD/<file>`), one `<s> <command>` a line, up to 64; blank lines and lines starting with `#` are left out, and a line may not hold a `\|`. The name is letters, digits, `.`, `_` and `-` only. The log says `Vita: vita_atMap from ux0:data/KisakCOD/<file>: N steps`. Read at start-up only. |
| `vita_assertExit 1` | A failed assertion or an exception nothing caught ends the process with exit code 1 where it would stop for a core dump. Vita3K hangs on that stop and loses the log's tail. Off by default: a console's dump is worth keeping. |

Every quit timer first prints all figures (`Vita: vita_quitAfterMenu 15
reached, last figures:`, then the load and memory lines, the sound's line and
the renderer's two `gpu memory` lines; the last three come from other code at
its next update, so their order is not fixed), waits a second and at least
three frames for those lines, and then quits (`Vita: quitting as asked`, 1 to
2 seconds after the `reached` line in Vita3K, with the engine's shutdown
lines after it). With a quit timer that is a multiple of `vita_statsEvery`
the last figures cover a whole period. The log also says when the events
happened: `Vita: the menu is up`, `Vita: the map is up`, and with the map
`Vita: script menus: 0 <name>, 1 <name>, ...`, the numbered script menus the
server named. After start-up it says what the start was asked to do, since it
does not hold the command line (`Vita: run switches: vita_quitAfter 600,
vita_quitAfterMenu 0, vita_quitAfterMap 30, vita_statsEvery 10,
vita_assertExit 1`, then `Vita: vita_atMap "..."` when one is set, or the
step file's name and step count).

An exception nothing caught (a `std::bad_alloc` when the heap is spent) logs
`Vita: ERROR: uncaught exception (std::bad_alloc); the process ends` and the
memory left, to the log and to standard output (an emulator's echo), and
syncs the log, with or without `vita_assertExit`. Every exit the port makes
itself closes `console.log` first: Vita3K does not implement the sync call
and holds a file's last 4 KB until it is closed, and after an exit code
other than 0 Vita3K's own process stays and has to be killed, so a runner
ends a start when Vita3K logs `The app exited with code N`. What neither
switch can end is a fault or a plain `abort()`: a runner still needs its own
time limit and a kill.

A menu start that is done 15 seconds after the menu shows:

    +set vita_quitAfterMenu 15 +set vita_quitAfter 300 +set vita_assertExit 1

A map start that picks a team and a class by itself and ends 30 seconds after
the map came up (the menu names are the ones the `script menus` line of an
earlier start lists; the responses are the stock scripts'):

    +devmap mp_crash +set vita_quitAfterMap 30 +set vita_quitAfter 600
    +set vita_assertExit 1
    +set vita_atMap "3 openscriptmenu <team menu> autoassign | 8 openscriptmenu <class menu> assault_mp,0"

`cmd mr {serverid} <number> <response>` sends the same response by the menu's
number (in the stock scripts 8 is the team menu and 15 the class menu; the
first run in Vita3K, 2026-11-10, spawned a player with `8 autoassign` and
`15 assault_mp,0`: a menu start took 30 s where the fixed-length one took
231, a map start 76 s where the clicked one took 426). The server answers
each with its own "Client ...'s snapshot" dump of about 40 lines, the
engine's note of the largest snapshot so far. A timed-command list that cannot be read is reported once (`Vita:
vita_atMap: part 2 ... does not start with a time in seconds`) and none of
its commands is sent.

A sweep that needs more than the command line holds goes in a step file:
one start can try every switch at both views. `+set vita_atMap "@sweep.txt"`
with `ux0:data/KisakCOD/sweep.txt`:

    # team and class
    3 cmd mr {serverid} 8 autoassign
    8 cmd mr {serverid} 15 assault_mp,0
    # facing the busy way, then without the world
    30 setviewpos 1264 -1896 124 90 0
    60 r_drawWorld 0
    80 r_drawWorld 1

`vita_atMenu` and `vita_atMap` are taken from the command line (`+set`) when
the game starts and not read again. A game server sets a client's dvars by
name, and what these two hold is run as console commands, so a value that
arrives later, from a server or from the console, sends nothing. The same
goes for the client's `vita_keyboardAnswer`.

### A list of starts

A console run compares several starts of one build, each with its own
command line and its own marker files in the program folder. Done by hand
every start costs a person an FTP round trip. With a list on the card one
tap on the bubble runs them all: `ux0:data/KisakCOD/runlist-mp.txt` for the
client (`runlist.txt` for the server), one start a line:

    # name | markers, or - for none | command line
    W1 | - | +devmap mp_crash +set vita_quitAfterMap 185 +set vita_atMap "@run-w.txt"
    W2 | no-frame-overlap | +devmap mp_crash +set vita_quitAfterMap 185 +set vita_atMap "@run-w.txt"
    W3 | no-vsync | +devmap mp_crash +set vita_quitAfterMap 185 +set vita_atMap "@run-w.txt"

At start-up the program takes the next line and, before anything else:

- writes the number of the start after it to `runlist-mp.at`, so a start
  that faults is not run again by the next tap; with the last start the list
  is renamed `runlist-mp.done.txt` and the next tap is an ordinary start;
- leaves the program folder (`gxp`) with this start's markers and no other:
  every empty file there without a dot in its name is removed, then the
  line's markers are made;
- writes its log to `console-mp-<name>.log` (the server: `console-<name>.log`);
- takes the line's command line. An empty one means the command line file,
  as in an ordinary start.

When the start ends, by its quit timer or by an error that exits
(`vita_assertExit 1`), the program starts itself again for the next line
(`Vita: run list: starting again for start 2 of 3` is the log's last line).
The restart call carries one argument that nothing reads: Vita3K carries
out the restart itself only for a call with arguments.
START + SELECT ends the start and the chain: the next line waits for the
next tap. A fault ends the chain too. So give every line a quit timer and
`vita_quitAfter` as the safety net.

A start after which the list goes no further (its last start, or one ended
by hand) removes every marker as it ends, so a list leaves the program
folder without markers and the next ordinary start is the plain build
(`Vita: run list: that was its last start; 1 markers removed`). Markers
that were there before the list are gone with it: make them again by hand.
A last start that faults cannot do this and leaves its markers.

The log's second line says which start it is and what was done to the
markers. A name is letters, digits, `.`, `_` and `-`; a marker lower-case
letters, digits and `-`; up to 32 starts and 8 markers a start. A list with
a line that cannot be read is left alone, the reason is logged and the start
is an ordinary one. The reading is `src/vita/vita_run_list.h`, checked on
the host by `scripts/tests/test_vita_run_list.py`.

### Two ways in one start

Some of the client's marker files can also be turned while the game runs,
by the console command `vita_gxm <marker> <0|1>`, where 1 is "as if the
marker were there". A step file can then measure the old way and the new
way minutes apart in one start, at the same temperature and battery state,
where two starts were needed:

    30 setviewpos 1264 -1896 124 90 0
    60 vita_gxm no-depth-in-scene 1
    90 vita_gxm no-depth-in-scene 0

The switches that can be turned: `no-depth-in-scene`, `no-resolve-skip`,
`no-depth-prepass`, `no-frame-overlap`, `no-vsync`, `gpu-scene-times` and
`no-draw-reuse`. The others size or make things at start-up and are refused
by name. A switch is turned between two frames and the log says so (`diag:
switch no-depth-in-scene is on from frame 1840 on (vita_gxm)`). The marker
files themselves are not touched. Leave a window at least 20 seconds and
drop its first 10 second line, which holds the frames of both ways.

### A bench that fetches its work

The list of starts still leaves a person two things per build: installing
the `.vpk` and carrying the files and logs by FTP. With one more file on the
card the multiplayer client fetches its own work from a computer on the same
network when it starts, and sends the logs back, so a build is tested with
one tap, or with none while the client waits for work.

On the computer (Python 3, standard library only):

    python3 scripts/platform/vita/vita_bench_server.py key --root ~/vita-bench
    python3 scripts/platform/vita/vita_bench_server.py serve --root ~/vita-bench --bind 192.168.8.213

`key` makes the folder and a key and prints the line for the card's
`ux0:data/KisakCOD/bench.txt`: the computer's address in numbers, the port,
the key, and optionally `wait <seconds>`:

    192.168.8.213 28970 5f0c...e1 wait 600

A job is queued with `add`: a run list, its step files, the names of files
to send back beside the logs, and the build to run it on, either a `.vpk`
from CI (its `eboot.bin` is taken out) or none for the installed build,
and optionally a pack of compiled programs (`--pack`, see "A pack of
programs" below):

    python3 scripts/platform/vita/vita_bench_server.py add --root ~/vita-bench --name run-w \
        --vpk KisakCOD-mp.vpk --run-list runlist-mp.txt --file run-w.txt

What a start of the client then does, before anything else:

- asks the computer for a job. None: an ordinary start (with `wait`, it asks
  again every 5 seconds for that long first; hold START + SELECT to end the
  wait; the screen stays black meanwhile);
- fetches the job's files into the data folder and its build to
  `ux0:app/<title id>/eboot-bench.bin`, each checked against the SHA-256 the
  job names. A build already there is not fetched again;
- runs the list: every start on the fetched build, which the installed build
  starts in its place. The installed `eboot.bin` is never changed;
- after the last start, back in the installed build, sends each start's
  `console-mp-<name>.log`, the files the job asked for and `bench.log`, tells
  the computer the job is over and asks for the next. With none left the
  process ends, where a person's tap would go on to the menu.

The computer keeps a finished job in `done/<job>/` with its `logs/` and a
`result.txt` (`ok`, or `failed:` and why); `list` shows what waits and what
is done. A job fails, and says so, when its run list cannot be read, a file
is not the one it names, the fetched build cannot be started, or it did not
come up in two tries; its list is then set aside, not run on the installed
build. A file the computer did not give (the Wi-Fi dropped) leaves the job
waiting for the next start. A start that faults ends the chain as it does
for any list; the next tap goes on, and after the last start sends what
there is. `ux0:data/KisakCOD/bench.log` says what the bench did, and the
start's own log repeats it in `Vita: bench:` lines.

What to know before leaving `bench.txt` on a card:

- It makes the Vita run a program it fetched over the network. The key in
  `bench.txt` and in the computer's `key.txt` is what limits that to the
  holder of the key: it never travels, every request and the job's
  description carry a code made with it (HMAC-SHA-256, the description's
  with a number the Vita picks for that one question), and the files are
  fetched by the SHA-256 the description names. Keep the key to those two
  files.
- Nothing is encrypted. Another device on the network can read the builds
  and logs that pass. Use it on a network you trust, bind the server to that
  network's address and stop it when the bench is not in use.
- A job writes `.txt` files with plain names into the data folder and the
  one build file, nothing else; it cannot name the game's data, a config or
  the bench's own files.
- With `bench.txt` on the card and no computer answering, every start of the
  client first waits up to 5 seconds (and the whole `wait`). Remove the file
  to have the client as it was. The address is of one network: at another
  place the file needs the computer's address there.

The dedicated server has no bench: its package may not write beside its own
`eboot.bin`. The Vita's side is `src/vita/vita_bench.h` (formats and codes),
`vita_bench_client.h` (network and files) and `vita_bench.cpp`; the first
two make no Vita call, and `scripts/tests/test_vita_bench.py` runs them on
the host against the real server script, playing the restarts.

### A pack of programs

The client compiles each shader program once and keeps it in a file of its
own in `ux0:data/KisakCOD/gxp`. Two things about that cost a console run
time: a build whose translator changed compiles everything again, a second
or so a program and over a thousand of them, and every start opens each
program's file on the card. With `programs.pack` in the program folder the
client looks in it first: one open for all of them, and the pack can come
from another machine. Vita3K runs the Vita's own compiler, so the folder a
PC run of the same build leaves holds the programs the console would
compile:

    python3 scripts/platform/vita/vita_pack_programs.py pack --out programs.pack <Vita3K's ux0>/data/KisakCOD/gxp

Copy `programs.pack` into the console's `gxp` folder, or give it to a
bench's job with `--pack` (above). A program is found in the pack by the
same key as its file, a hash of its source, so a pack from another
translator is not used and does no harm; a program the pack lacks is read
from its own file or compiled as before, and what is compiled still goes to
a file. Folders and packs can be packed together, the later source winning.
The quit lines say `program pack ...: programs.pack holds N programs; M of
the K read from the card came out of it`. The programs come from the game's
shaders, so a pack stays on the owner's machines like the game's files.
The reading is `src/gfx_gxm/gxm_program_pack.h`, checked on the host by
`scripts/tests/test_vita_program_pack.py`.

## Testing in Vita3K

Vita3K boots the `.vpk` far enough to check start-up, files, threads and,
with game data, map loading. It does not model the Vita's memory limit (it
leaves the program's own size out of its free memory) or its CPU speed, so
the memory budget and the load figures still need the console.

In a headless Linux sandbox, the Vita3K AppImage (continuous release) works
with software OpenGL on Xvfb:

```sh
./Vita3K-x86_64.AppImage --appimage-extract && ln -s squashfs-root AppDir
Xvfb :91 -screen 0 1280x720x24 +extension GLX &
cd AppDir && DISPLAY=:91 XDG_RUNTIME_DIR=/tmp/xdg LIBGL_ALWAYS_SOFTWARE=1 \
  MESA_GL_VERSION_OVERRIDE=4.6COMPAT MESA_GLSL_VERSION_OVERRIDE=460 \
  LIBGL_DRIVERS_PATH=$PWD/lib/dri \
  ./AppRun --frontend sdl -B OpenGL /path/to/KisakCOD-dedi.vpk
```

The emulated `ux0:` is `~/.local/share/Vita3K/Vita3K/ux0`, so the log is at
`ux0/data/KisakCOD/console.log` there. Without game data, copy the stub tree
that `scripts/tests/test_linux_bootstrap.py` makes (two small `.iwd` files and
a one-line `fileSysCheck.cfg` in `main/`) to get as far as the first zone.

## Finding alignment faults before a console run

Vita3K and x86 PCs read a float or an 8-byte value at any address, so the
alignment faults described under "Alignment" below only show on the console,
one crash per run. The i386 Linux server has the same structure layouts as
the Vita build (4-byte pointers, little-endian), so it can find them first: a
build with `-DKISAK_ALIGNMENT_CHECK=ON` compiles KisakCOD-dedi with UBSan's
alignment check, which reports every load and store at an address its type
is not aligned for, once per source line, and carries on.

```sh
cmake -S . -B build-linux/align -G Ninja \
  -DKISAK_PLATFORM=linux -DCMAKE_BUILD_TYPE=RelWithDebInfo \
  -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++ \
  -DKISAK_ALIGNMENT_CHECK=ON \
  -DCMAKE_RUNTIME_OUTPUT_DIRECTORY="$PWD/build-linux/align/bin"
cmake --build build-linux/align --target KisakCOD-dedi
mkdir -p ~/kisak-private/align
export UBSAN_OPTIONS=log_path=$HOME/kisak-private/align/ubsan:print_stacktrace=1
# then run the server: the asset start-up test on each map, the bots test with
# --cycles, or a server by hand
python3 scripts/tools/summarize_alignment_report.py ~/kisak-private/align/ubsan.*
```

Each process writes its own `ubsan.<pid>`. The summary sorts the reported
source lines into three tables: **faults** (a float, double or 64-bit access
at an address that is not a multiple of 4, which crashes the Vita), **check**
(a structure, union or enum reached through a misaligned pointer: it faults
only if the code reads a float or 8-byte member through it), and **fine** (word
aligned, or a single int, short or pointer, which the Cortex-A9 handles). It
exits 1 when anything faults. The report holds source lines and addresses
only, no game data.

What it cannot see: code the run never reaches, the Vita-only code in
`src/vita/`, and VitaSDK's own libraries. UBSan keeps
a few client-only functions in the link that the normal server drops; they
are never called, and `scripts/platform/linux/alignment_check_stubs.txt`
defines their symbols as address 0 so the link completes; if a later change
makes the link fail on another undefined client symbol, add its mangled name
there. The build defines `KISAK_ALIGNMENT_CHECK`, so code with a separate
32-bit ARM path (the script VM's vector constants) takes that path and the
report shows what the Vita build would do.

First scan (laptop, 2026-11-08, all 21 stock maps plus bots, before the
build took the ARM vector path): 35 source lines. The 10 that fault are all
reads of script vector constants inside the bytecode (`Scr_EvalPlus`,
`Scr_EvalMinus`, vector indexing, `Scr_GetVector`): the console run 3 crash,
already fixed on ARM. The 4 to check are `CM_AddStaticModelToNode`, which
treats a sector's 2-byte model list head as a `cStaticModel_s` but only reads
and writes its 2-byte first member (one halfword access, fine). The rest are
word and halfword operands in the packed bytecode (fine).

## How the port works

### Toolchain: Clang compiles, VitaSDK links

`scripts/platform/vita/vita-clang.cmake` compiles every unit with Clang for
`armv7a-none-eabihf`, `-mcpu=cortex-a9 -mthumb`, against VitaSDK's newlib and
libstdc++ headers, and links with `arm-vita-eabi-g++` (VitaSDK's crt files,
linker script and stub libraries). `-Wl,-q` keeps the relocations that
`vita-elf-create` needs; `vita-make-fself` and `vita-pack-vpk` finish the
`.vpk` (`scripts/platform/vita/CMakeLists.txt`). The ABI points:

- **Floats.** Hard-float calls and scalar VFPv3 only (`-mfpu=vfpv3`), no
  fused multiply-add (`-ffp-contract=off`), signed `char`: the armv7 rules,
  so the server's movement and collision maths give the same results as on a
  PC. The whole binary, VitaSDK's libraries included, has no NEON float
  instruction (CI checks). At start-up each thread clears the FPSCR
  flush-to-zero and default-NaN bits (`Vita_SetIeeeFloatMode`); the log
  records what the system had set.
- **Thread-local storage.** VitaSDK's GCC is built without native TLS, and the
  Vita loader has no TLS segment, so Clang uses `-femulated-tls`, the same
  `__emutls_get_address` scheme as GCC's libgcc. libgcc keeps one copy per
  thread only if its weak references to `pthread_cancel` and `pthread_once`
  resolve; nothing else in the link uses them, so they are pulled in with
  `--undefined`. Without them every `thread_local` (the engine's thread
  context among them) is shared by all threads: the second console run's
  database thread switched the main thread's context, and the main thread
  then crashed on another thread's empty trace data. CI fails on any weak
  `pthread_` reference left unresolved, and at start-up the server checks
  that a second thread sees its own copy, and stops if not.
- **Enum size.** VitaSDK's libraries use short enums (`Tag_ABI_enum_size:
  small`); the engine keeps 4-byte enums, because its retail structures (and
  the fastfile layout checks in `db_disk_layout.h`) need them. This is safe at
  the boundaries the server uses: VitaSDK's own structure size checks
  (`ENABLE_VITASDK_BUILD_ASSERT`, on for this build) pass under Clang for
  every system header the server includes;
  libstdc++ sizes its stream flag enums to 4 bytes on purpose; the scoped
  enums (`std::chars_format`, `std::filesystem` options, `std::errc`) have a
  fixed `int` type; and the one unscoped enum in a shared structure, the
  red-black tree node colour behind `std::map`/`std::set`, is only read inside
  libstdc++ (1-byte reads of the low byte, which the engine's 4-byte writes
  set correctly on a little-endian CPU). The linker's enum-size warning is
  therefore turned off. Re-check this list when the server starts using more
  of the C++ library.
- **Fastfiles.** Pointers are 4 bytes and the CPU little-endian, so PC
  fastfiles load unchanged, as on i386 and armv7 Linux.
- **Alignment.** The Cortex-A9 handles an unaligned single load or store
  (`LDR`/`STR`), but faults on the paired and multiple forms
  (`LDRD`/`STRD`/`LDM`/`STM`, and `VLDR`) at an unaligned address. Linux
  fixes these up in the kernel; the Vita does not, and Vita3K does not model
  the fault. The engine reads and writes words at any address (decompiled
  byte-buffer code), so every unit is compiled with
  `-mllvm -arm-assume-misaligned-load-store`, which stops Clang pairing two
  word accesses into one. Copies of whole 8-byte structures still use the
  paired forms; they are safe while those structures stay aligned. Float
  loads and stores (`VLDR`/`VSTR`) need 4-byte alignment on every ARMv7, so
  the script compiler and VM copy float operands out of the packed bytecode
  with `memcpy`, and on 32-bit ARM `OP_GetVector` pushes an aligned,
  reference-counted copy of the constant instead of a pointer into the code.
  A crash
  with stop reason `0x30004` (data abort) on the console is the first thing
  to check for this; "Finding alignment faults before a console run" above
  finds them on a PC.
- **Main thread stack.** `sceUserMainThreadStackSize` (8 MB, in
  `vita_main.cpp`) is read only by `vita-elf-create`, so the link keeps it
  with `--undefined`; `--gc-sections` dropped it in the first build and the
  main thread got the 256 KB default.

### Platform layer

The Vita build compiles the POSIX layer in `src/linux/` (newlib offers the
same calls as Linux for files, sockets, threads and time) and adds
`src/vita/` for what the Vita lacks. Engine sources only gained
`|| defined(__vita__)` on their existing POSIX guards, plus a Vita branch for
the challenge random number (`sceKernelGetRandomNumber`).

| Area | On the Vita |
| --- | --- |
| Virtual memory | No API for it. `src/vita/vita_mman.cpp` backs `mmap`, `mprotect`, `madvise` and `munmap` with memory blocks: a reserve allocates its full size at once (zeroed), protection changes do nothing, a decommit zeroes the range. |
| Memory budget | See "Memory" below. |
| Start-up | `Vita_Startup` (`src/vita/vita_main.cpp`): CPU to 444 MHz and bus to 222 MHz, data folder `ux0:data/KisakCOD` created and made the working directory, console log opened, network started (module, `sceNetInit` with a 1 MB pool, `sceNetCtlInit`), command line read from `cmdline.txt`. |
| Paths | `fs_basepath` defaults to `ux0:data/KisakCOD`. Paths with a device (`ux0:`) count as absolute. Directory entries carry their `SceIoStat` instead of `d_type`; the tree removal walks by path (no symbolic links on the Vita). |
| Files | newlib's `off_t` is 32-bit: positional reads and writes past 2 GiB fail instead of wrapping. The stock fastfiles and IWDs are far smaller. |
| Network | BSD sockets through newlib. No `getifaddrs`: the local addresses are loopback and the Wi-Fi address from `sceNetCtlInetGetInfo`. No `MSG_TRUNC`: a datagram that fills the receive buffer is dropped as oversize. |
| Threads | newlib pthreads. Worker threads get a 1 MB stack (the Windows default); they cannot be renamed. Three cores are reported to the engine (applications run on three of the four). |
| Time | `__rdtsc` reads `sceKernelGetProcessTimeWide` (microseconds). |
| Console | No terminal and no stdin: output goes to `console.log`, commands come from `cmdline.txt` and rcon. |
| Quit | No signals: START + SELECT queues `quit`. The server also keeps the console from auto-suspending. Quit and fatal errors end the process with `sceKernelExitProcess`: newlib's `exit` frees the heap before the kernel stops the other threads, and a worker running in that gap faults (Vita3K showed it). |
| Steam | Off. Admission is unchanged (retail rules; see the Linux guide). |

### Memory

The `.vpk` asks for extended memory (`ATTRIBUTE2=12`): 365 MB of main memory
for the application. The Vita has no virtual memory, so everything below is
in memory from launch:

| Part | Size |
| --- | --- |
| Program and its static data | ~87 MB, of which `svs`, the server's snapshot buffers, is 70 MB |
| newlib heap (`_newlib_heap_size_user`) | 64 MB (newlib would take 128 MB) |
| Engine reservations (`vita_mman.cpp`) | 128 MB zone arena, 10 MB hunk, 16 MB debug hunk, small listing hunks |
| Stacks and network pool | main thread 8 MB, workers 1 MB each, network 1 MB (static) |

That leaves about 45 MB. On the console the system reported 208 MB of
main memory free once the server had started (Vita3K reports 301 MB, because
it leaves the program out). In retail `svs` is 178 MB: its rings are sized for
64 clients (64 `client_t` of 662 KB, delta-snapshot entity and client states,
and the cached snapshots behind killcam). The Vita build sets `SV_MAX_CLIENTS`
(`bg_local.h`) to 24 and the rings scale with it (`server_mp.h`): the same
history per client, for fewer clients. Every other build keeps the retail
sizes (the i386 `svs` is unchanged). The laptop memory run puts mp_crash's
zones at 82 MB of the 128 MB arena; the heap held 0.2 MB at the first zone in
Vita3K, so its size is set again from the first map loads.

Not built for the Vita: `src/linux/sl_trace.cpp` (glibc backtraces), and the
Linux runtime self-tests, which fork and check memory protection faults the
Vita cannot raise.

## Next steps

1. Run 0, 4 and 8 bots at `sv_fps 20` and read the load lines; size the
   player cap, the heap and the zone arena from them.
2. More players and other maps; rerun the alignment scan after big engine
   changes.
3. Commit the reservations lazily (the 16 MB debug hunk is rarely used) if
   memory is short.
4. Optimisations from the feasibility study (bots, collision, script VM).
