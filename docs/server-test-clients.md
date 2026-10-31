# Server-side test clients

## Current result

The multiplayer server already provides a GSC `addtestclient()` function. It
creates an internal `NA_BOT` slot and the server generates user commands for it.
This is useful for testing server-side slot and command behavior without another
Steam copy. It is **not** a second real client, a client decoder, or a navigating
AI.

The asset-free function tests pass. The installed-assets probe described below
is **not currently a passing gameplay regression**: three bots are admitted,
but the stock-script fixture kicks them before its end-of-hold checks. Those
assertions remain enabled and the probe reports failure. Do not use it as proof
of sustained bot gameplay or multi-player synchronization.

## Existing implementation

- `GScr_AddTestClient` in `src/game_mp/g_scr_main_mp.cpp` exposes
  `addtestclient()` to scripts and returns the created entity.
- `SV_AddTestClient` in `src/server_mp/sv_client_mp.cpp` checks capacity,
  constructs a protocol-1 connect with a distinct bot port, passes an `NA_BOT`
  address to `SV_DirectConnect`, marks the resulting slot as a test client,
  sends gamestate and enters the world.
- `NET_IsLocalAddress` in `src/qcommon/net_chan_mp.cpp` treats `NA_BOT` as a local
  pseudo-address. This is the existing internal admission path, not a remote
  authentication bypass.
- `SV_UpdateBots` / `SV_BotUserMove` in `src/server_mp/sv_main_mp.cpp` generate
  randomized commands. `sv_botsPressAttackBtn` controls the attack bit; these
  clients have no pathfinding, aiming or objectives.

The loopback authentication test mode deliberately rejects pseudo-addresses.
Do not enable `sv_loopbackTestAuth` for this bot probe. Leave both authentication
opt-ins off; the diagnostic script checks their effective values in GSC.

## Asset-free regression

```sh
python3 scripts/tests/test_server_test_clients.py --cxx clang++-19 --m32 --sanitize
# Optional negative-control builds against temporary mutated source copies:
python3 scripts/tests/test_server_test_clients.py --cxx clang++-19 --m32 \
    --sanitize --mutations
```

The Linux check runner includes the first command as `server_test_clients`.
It extracts the production `SV_AddTestClient`, `SV_BotUserMove` and
`SV_UpdateBots` bodies. It checks capacity refusal, constructed addresses,
command fields, mixed occupied/free slots, connection-boundary rejection,
per-slot command dispatch and reuse after a test externally frees a slot.

Important boundaries:

- `SV_DirectConnect` is a double in this harness. It does not test real
  authentication, reserved-slot policy or actual connection admission.
- Slot reuse is not a test of `SV_DropClient` or script teardown.
- User-command types match the relevant multiplayer declaration, but this is
  not a wire-serialization or complete ABI test.
- Random inputs are controlled branch inputs, not AI or randomness-distribution
  validation.
- Automatic-variable pattern initialization makes omitted address initialization
  observable; the script probes compiler support and refuses an ineffective
  configuration. ASan/UBSan builds disable recovery so diagnostics fail the run.

This test exposed unaligned word-sized stores in the original
`SV_AddTestClient` address construction. The implementation now zeroes each
complete address and assigns its type/port explicitly. Address semantics,
capacity and authentication policy are unchanged.

## Installed-assets diagnostic probe

Build a separate server output so a running session's binary is not replaced:

```sh
cmake -S . -B build-bots -G Ninja -DKISAK_PLATFORM=linux \
    -DCMAKE_BUILD_TYPE=RelWithDebInfo \
    -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++ \
    -DCMAKE_EXPORT_COMPILE_COMMANDS=ON \
    -DCMAKE_RUNTIME_OUTPUT_DIRECTORY="$PWD/build-bots/bin"
cmake --build build-bots --target KisakCOD-dedi --parallel 2
python3 scripts/tests/test_linux_bots.py "/path/to/Call of Duty 4" \
    --binary build-bots/bin/KisakCOD-dedi --bots 3 --timeout 45 --keep
```

**Expected current result: nonzero**, with retained logs explaining the failed
hold checks. This probe is not run in public CI or the asset-free runner.

It uses a fresh temporary home/mod, symlinks installed assets read-only, binds
an ephemeral `127.0.0.1` port and never edits the game installation. Because
fastfile gametype validation only accepts registered names, the fixture
overrides `dm.gsc` in its private mod and delegates initialization to stock
`war::main`. This is a diagnostic fixture, not a stock DM round. Its markers
use `logprint` and `g_logSync`, avoiding developer-only `println` output.

Observed in the three-bot run on `mp_crash`:

1. GSC recorded `sv_loopbackTestAuth=0` and `sv_allowUnverifiedGuids=0`.
2. Entities 0, 1 and 2 were created; the server recorded connection admission,
   gamestate generation and player snapshots for the internal clients.
3. The game log recorded joins at approximately one second, then departures
   at two to three seconds, before the four-second hold marker.
4. Server output recorded `EXE_PLAYERKICKED` / `was kicked for cheating` for
   the bots. The retained GSC references were then undefined.

The final probe reached its marker, but the server also failed to finish
shutdown within the 45-second cleanup deadline and was killed by the harness.
It recorded `quitting...` and entered `ShutdownGame`; that is not a clean-exit
pass. The isolated logs were retained for both failures.

The native message does not establish the precise script condition that caused
the kick. That cause remains open; it is not a generic entity-reference-lifetime
failure or proof that the bots survived until shutdown. Changing authentication
or ignoring the kicks would not constitute a fix.

Next useful investigation: capture the script-triggered kick context and use the
actual stock team/class connection callbacks, rather than forcing session state
and invoking `spawn` directly. Only after a sustained, healthy hold passes should
bot death/respawn, real drop/refill and map-transition checks be added. None of
those later actions is currently verified by this probe.
