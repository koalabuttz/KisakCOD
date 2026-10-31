# Reliability checkpoint — 2026-10-31

Base: `f8bde51a` from `koalabuttz/KisakCOD` master. Local branch:
`dot/server-reliability-2026-10-31`. Nothing pushed or merged.

## Changes and causal evidence

- `bd4f6d64`: dedicated mod shutdown now preserves database-owned weapon
  strings. `BG_LoadWeaponDef` always returns fastfile definitions on dedicated
  builds, while teardown previously treated nonempty `fs_game` as raw ownership.
  A Debug baseline aborted in `SL_FreeString` (`!refStr->user`), reached from
  `BG_FreeWeaponDefStrings` / `Com_FreeWeaponInfoMemory` / `G_ShutdownGame`.
  With only the engine fix, the old bot fixture still failed its hold/kick
  checks but exited zero. The ownership regression fails on the old source.
  Client raw-file cleanup now follows the same mod-or-raw-mode condition as
  its loader, including raw mode without a mod.
- `65d7cf58`: internal bots use the stock `pers["isBot"]` convention and normal
  `menuresponse` team/class callbacks (`allies`, `assault_mp,0`). No direct
  session-state assignment, direct spawn, fake player stats or auth change.
  Private diagnostic logging in the installed stock script confirmed each old
  fixture kick came from zero class stat 205. That diagnostic script is not
  included in the repository. The fixture now requires the exact admitted set
  in every one-second hold sample, positive health, playing state, selected
  team/class and an equipped weapon. It also reads the engine console log when
  checking script errors, since stdout alone omitted important diagnostics.
- `7225e838`: lifecycle transitions share one full timeout budget. An early
  same-map `getinfo` reply previously left `getstatus` with a five-second limit
  while the restart was still loading. Steady-state hold queries retain their
  five-second responsiveness requirement. Incorrect replies still fail.

## Verification

The saved environment cannot directly execute ELF32/i386 (`Exec format error`).
All 32-bit execution below used **QEMU user-mode i386 10.0.13**, not native
kernel execution. Clang 19 and multilib libraries were extracted locally into
`/workspace/native-tools`; system package installation was unavailable.
A local, uncommitted Python subprocess adapter prepended QEMU to ELF32 children.
The server additionally used `TRACY_NO_INVARIANT_CHECK=1` because QEMU does not
advertise invariant TSC; this changes the profiler check, not game assertions.
No proprietary assets were obtained; tests used the already-installed assets.

| Check | Result |
| --- | --- |
| Debug and RelWithDebInfo dedicated builds | Passed, isolated outputs |
| Full default 17-check Debug attempt | **14 passed, 3 failed/blocked**, detailed below |
| Final applicable subset, both builds | **14/14 passed** on each; skips explicitly listed below |
| Weapon ownership | 40 cycles per dedicated/client variant; emulated 32-bit and native 64-bit fatal ASan/UBSan passed; pre-fix negative control failed |
| Bot evidence validator | 5 unit tests passed, including duplicate/missing/dead/unequipped evidence |
| Lifecycle driver | 32 unit tests passed |
| Runner / fastfile compiler driver | 24 / 11 unit tests passed |
| Live bots, Debug `mp_crash` | Three bots, six samples over five seconds, all 17 checks passed, exit zero |
| Live bots, release `mp_crash` and `mp_shipment` | Three bots, 11 samples over ten seconds per map, all 17 checks passed on each, exit zero |
| Empty-server lifecycle, both builds | Three cycles each: restart Crash, change to Shipment, rotate to Crash; two-second holds, fresh UDP replies, console quit and separate SIGTERM exit zero |
| UDP suite, explicit QEMU execution | 264 checks passed |
| Handshake, unsanitized 32/64-bit | All four configurations passed (729 dedicated / 669 nonnative checks per width) |
| Handshake, native 64-bit fatal ASan/UBSan | Both configurations passed |
| Bot functions | Native 64-bit fatal ASan/UBSan and unsanitized emulated 32-bit passed |

The full default attempt was **not green**:

1. `handshake_auth` timed out at its 300-second runner deadline in emulated
   32-bit ASan execution.
2. `udp` builds correctly but its shell script directly executes ELF32; this
   kernel rejects that. Running that same generated binary explicitly through
   QEMU passed all 264 checks.
3. `server_test_clients` also stalled in 32-bit ASan initialization. Its owned
   process group was stopped after an independent small ASan executable had
   reproduced a pre-main QEMU timeout. The runner records failure (SIGTERM),
   not a test pass.

The final 14-check runs explicitly skipped these three entries. Unsanitized
emulated and native 64-bit sanitized supplements are useful evidence, **not**
replacement proof for the missing native 32-bit sanitizer run. CI assertions,
sanitizers, default runner entries and deadlines remain enabled.

Same-map QEMU-process RSS growth after warm-up was at most 8 KiB in Debug and
4 KiB in release, with zero descriptor/thread growth. These describe this
bounded empty-server test, not native performance or proof of leak freedom.
One debugger-attached QEMU attempt also asserted `!g_loadingAssets` during map
loading; ordinary repeated runs did not reproduce that. It remains an observed,
unresolved diagnostic result, not a repaired engine race.

## Reproduction and retained evidence

On a native multilib host, use the normal commands in `docs/linux-dedicated.md`
and `docs/server-test-clients.md`. Run the full default suite without skips:

```sh
python3 scripts/tests/run_linux_checks.py --build-dir BUILD --binary BUILD/bin/KisakCOD-dedi --cxx clang++-19
python3 scripts/tests/test_linux_bots.py /path/to/installed/game --binary BUILD/bin/KisakCOD-dedi --hold-seconds 10 --keep
python3 scripts/tests/test_linux_server_lifecycle.py /path/to/installed/game --binary BUILD/bin/KisakCOD-dedi --output-dir NEW_DIRECTORY --cycles 3 --hold-seconds 2 --shutdown both
```

Local logs and JSON reports are retained in
`/workspace/kisakcod-reliability-results/`, including the failing baseline,
engine-only repair, kick-context evidence, final bot runs, default suite attempt,
both final subset suites and both lifecycle reports. Toolchain/QEMU wrappers and
build logs remain in `/workspace/native-tools/`. These generated files, binaries
and licensed scripts/assets are not committed.

## Next work

First rerun the complete 17-check suite and live checks on a host with native
32-bit execution, including sanitizer coverage. Then extend the bot fixture to
real death/respawn, explicit disconnect/refill and map transitions with bots.
Current rotation evidence has **no connected players**; current bot evidence
has no map transition. No two-human, mixed-retail/CoD4x or long gameplay result
is claimed.

The next separate native-client branch should execute production
`CL_ParseCommandString` with production message readers in the existing opt-in
client-core harness. Inspection confirms it commits a newer sequence/string
without checking `msg->overflowed`, while `MSG_ReadString` turns a failed read
into a terminator. Start with a failing truncated-command regression, then
cover duplicate/stale sequences, ring wrap, bounded strings, truncated sequence
and missing terminator before changing behavior. This is a concrete lead, not
runtime-verified client behavior in this checkpoint. Keep the default-OFF and
untouched-dedicated-binary checks in both build configurations. Renderer,
window/input integration and Steam authentication remain separate work.
