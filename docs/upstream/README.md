# Upstream patch queue

Changes from this fork prepared for the upstream repository, one patch per
future pull request. **Nothing here has been sent upstream.** The queue is
preparation only; filing anything is David's decision.

Each patch is made on upstream's own tree, not cut from the fork's history, so
it carries a short message of its own and none of the fork's tests, tools or
platform code.

## Layout

- `BASE`: the upstream commit every patch is made on.
- `patches/NNNN-topic.patch`: one `git format-patch` file per future pull
  request. Each applies to `BASE` alone, unless `DEPENDS` lists it.
- `DEPENDS`: the few patches made on top of another patch. A line
  `0015: 0014` says 0015 can only be offered once 0014 is in.

## The queue

| Patch | Fixes | Fork change it comes from |
| --- | --- | --- |
| 0001 | `rcon` is accepted with an empty password when the server has not set `rcon_password` | security fixes merge `52fdd24` |
| 0002 | The flashbang white-out and the concussion blur never show in multiplayer | commit `57dec53` |
| 0003 | `where` and the invalid-map vote reply format with missing arguments | commit `f662443` |
| 0004 | Color dvars registered with red above zero start at red 1/255 | commit `fcb454d` |
| 0005 | A server listing more than 32 fastfiles makes the client write past two 32-entry arrays | commit `79cf6ff` |
| 0006 | The "Invalid command time" message uses text holding the player's name as its format | security fixes merge `52fdd24` |
| 0007 | A retry after a refused connect is dropped by the reconnect throttle | commit `8e5317e` |
| 0008 | The cached tag name's string reference is lost on every map change | commit `5195157` |
| 0009 | `deactivateReverb` and `deactivateChannelVolumes` send a format the client does not read | commit `f662443` |
| 0010 | The script debugger reports a known field as unknown when its id has a zero low byte | commit `b39e6b9` |
| 0011 | Every operator of a menu expression scans an 84-entry list for its handler (upstream issue 27) | commit `9a5768a` |
| 0012 | A script with 32 chained `else if` branches or 200 nested parentheses crashes the script compiler | commit `31370c7` |
| 0013 | Script parser error recovery reads in front of a grammar table | commit `27a760f` |
| 0014 | Four `DvarFlags` names do not match what their bits do; two are swapped (upstream issue 30) | commits `d813766`, `f8ffa8b` |
| 0015 | Dvars a script marks as serverinfo never reach the multiplayer serverinfo string | commit `88103e0` |
| 0016 | With `sv_allowDownload` on, a client can download any file under the server's home path, such as its config | security fixes merge `52fdd24`, written smaller |
| 0017 | The OpenAL driver detaches a buffer from a source that is still playing, so cut sounds keep playing and the next one is lost (upstream issue 76) | commit `0b48d2e` |
| 0018 | An OpenAL stream that runs out of queued audio stays silent (upstream issue 76) | commit `0b48d2e` |
| 0019 | The 16-bit angle factors are bare numbers in 16 files (upstream issue 53) | commit `7fc5013` |
| 0020 | 1/360 is a bare number on 90 lines of 23 files (upstream issue 45) | commit `8569b17`, first half |
| 0021 | The wrap of an angle into [-180, 180] is written out 19 times (upstream issue 45) | commit `8569b17`, second half |
| 0022 | The origin codec rounds the map centre with a bare `+ 0.5` and a cast in six places (upstream issue 56) | commit `431c9d9` |
| 0023 | Eight vector snaps are three `(float)(int)` casts each (upstream issue 52) | commit `f783b2e` |
| 0024 | Unbounded string calls in `universal`, `qcommon`, `script` and `stringed` (upstream issue 8, part 1 of 4) | commit `85316f4` |
| 0025 | Unbounded string calls in `game`, `game_mp`, `server` and `server_mp` (part 2) | merge `354d177` |
| 0026 | Unbounded string calls in `client`, `client_mp`, `cgame`, `ui` and `ui_mp` (part 3) | merge `b415b29` |
| 0027 | Unbounded string calls in `gfx_d3d`, `win32` and `sound` (part 4) | commit `acc81f6` |

Patches 0001 to 0004 are the proposed first batch and 0005 to 0010 the
second. 0011 to 0015 answer open upstream issues or sit next to one, and
come after those. 0016 belongs with the second batch; 0017 and 0018 go with
the other fixes for open issues. 0019 to 0021 are the first of the cleanup
issues; they form a chain (0021 on 0020 on 0019) because all three change
neighbouring lines of the same angle code. 0022 and 0023 are the rounding
issues; neither changes behaviour. 0024 to 0027 are upstream issue 8 by
directory; each applies to the base alone, so they can go in any order.

Open points on single patches, to settle before each is offered:

- 0011 gives the same results as the scan, but its effect on menu frame time
  has not been measured. Upstream issue 27 comes with profiler pictures, so
  it needs a profile of a real menu first.
- 0012 may be the crash in upstream issue 89 (mods that crash a listen
  server): mod scripts often hold long `else if` chains. Nobody has run the
  two mods named there, so the patch does not claim it.
- 0014 is a rename across 16 files and has to be regenerated if upstream
  touches a dvar registration first. Every use keeps its numeric value.
- 0015 rests on retail 1.7 `iw3mp.exe` passing `0x404` as the mask. It has
  not been run against a live server.
- 0016 is stricter than retail 1.7, which has the same hole. The message
  says so.
- 0017 and 0018 change code upstream compiles only with `KISAK_OPENAL` on,
  which is off there, so the hosted build of the queue does not compile
  them. Both were run against OpenAL Soft's loopback device with upstream's
  own driver code.
- 0019 uses the fork's names (`DEGREES_PER_SHORT`, `SHORTS_PER_DEGREE`). On
  upstream issue 53 one maintainer suggested writing the first as `45/8192`
  and a contributor suggested Quake's `SHORT2ANGLE` and `ANGLE2SHORT` macros.
  Quake's `ANGLE2SHORT` formula would change results, because the game
  multiplies by the float factor; the names can still be changed before this
  is offered. Upstream issue 59 was a duplicate of 53 and is closed.
- 0020 and 0021 split upstream issue 45 in two so the rename, which cannot
  change a result, can go in without the fold. The issue's title asks for
  `AngleNormalize180()`. Upstream's `AngleNormalize180()` uses `fmodf`, and
  over every float it gives a different last bit than the inlined wrap for
  most inputs (up to 0.00006 degrees between -720 and 720), so 0021 adds
  `AngleWrap180()` with the inlined body instead. Giving
  `AngleNormalize180()` that body and one name is the other way to do it; it
  changes the three places that call it today. That choice is the
  maintainers'.
- 0021 leaves written out: the 36 lines that multiply by the double factor
  (two inputs out of 2^32 differ from the float form), one vehicle wrap that
  adds a double 0.5 before the floor (about 8.4 million inputs differ), one
  that scales by 7200, and the single-player actor, turret, vehicle path,
  pose and model previewer copies.
- 0022 keeps the rounding upstream issue 56 asks to reconsider, and says
  why: retail 1.7 adds 0.5 and truncates, and both ends of the origin codec
  have to agree on the centre. The issue's author may still want
  `std::round`; that would break play against retail clients and servers on
  maps whose centre has a negative coordinate with a fraction below one
  half.
- 0023 gives upstream issue 52 the retail addresses that show the game's
  vector snaps truncate. Upstream already switched the rounding sites to
  `SnapFloat()` itself and left the issue open; this patch only names what
  is left. A contributor also named `SnapAngles` and
  `SnapPointToIntersectingPlanes` on the issue; the fork has not checked
  those two against retail.
- The retail addresses in 0022 and 0023 come from the fork's scan of a
  retail 1.7 `iw3mp.exe` (`scripts/audit/retail_float_int_scan.py`).
- 0024 to 0027 are the fork's four changes for upstream issue 8, applied to
  upstream's files. Two files are left out: `qcommon/dl_main.cpp`, where
  the fork's version differs too much, and `client_mp/cl_connection_mp.cpp`,
  which upstream does not have. `src/radiant` (about 250 sites) is not
  covered. Where a text did not fit its buffer the old code overran it; the
  new code cuts the text. On the issue a maintainer wrote that they would
  post a list for a future pull request and has not yet, so ask before
  offering these. At 30 to 110 changed lines each they are larger than
  upstream usually takes; each can be split further by file.
- Upstream issue 36 needs nothing from the fork: upstream has already
  commented out the copies it names.

Fork fixes looked at and kept back for now:

- The `iprintln` command buffer, the deterministic Huffman tree, the 15-bit
  random helpers and the script scanner's end-of-file rule only matter off
  MSVC, so they travel with a port.
- The voice start-up clean-up, the two script watchdog changes and the VM
  field stack change are each larger than upstream takes in one piece, and
  none is a known fault in upstream's Windows build.
- The saved-dvar restore and the shellshock view-kick fix change behaviour
  and need evidence from the retail game first.

## Rules

`scripts/tools/upstream_queue.py lint` checks these; the workflow
[`upstream-queue.yaml`](../../.github/workflows/upstream-queue.yaml) runs it.

- The subject uses upstream's style: `fix:`, `security:`, `refactor:` and so
  on, then a plain phrase.
- The body is at most 12 lines: what was wrong, the fix, how it was checked
  when that is not obvious.
- No issue or pull request number written with a hash sign, no fork pull
  request numbers, no links into the fork, no notes about the fork's test
  machines, no session trailers. Write an upstream issue as "upstream issue 52".
- The smallest change that fixes the fault. No fork-only paths (`src/linux`,
  `scripts/tests`, `scripts/tools`, `docs`).

## Checks

```sh
# Rules only; needs nothing but this checkout.
python3 scripts/tools/upstream_queue.py lint

# Rules, and that every patch applies to the pinned base alone (or on the
# patches DEPENDS names for it) and all apply in order. Fetching upstream is a
# read of a public repository.
git init -q /tmp/upstream
git -C /tmp/upstream fetch -q --depth 1 https://github.com/SwagSoftware/KisakCOD.git "$(cat docs/upstream/BASE)"
git -C /tmp/upstream checkout -q --detach FETCH_HEAD
python3 scripts/tools/upstream_queue.py check --upstream /tmp/upstream
```

The hosted workflow does the same and then builds the patched upstream tree
with MSVC (Debug and Release; sp, mp and dedi), using the steps of upstream's
own workflow. That build is of all patches together.

## Adding a patch

1. Check out upstream at `BASE` in a scratch directory.
2. Make the change there and commit it with a message that keeps the rules.
3. `git format-patch -1 --zero-commit -N --no-signature --stdout > docs/upstream/patches/NNNN-topic.patch`
4. Add a row to the table above and run the checks.

A patch that cannot be made on `BASE` alone, because it changes lines another
patch changes, is made on top of that patch's commit instead and gets a line
in `DEPENDS`. Keep those rare: every one is a pull request that has to wait
for another.

When upstream moves, update `BASE` and refresh any patch that no longer
applies.
