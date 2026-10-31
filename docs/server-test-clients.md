# Server-side test clients

## Current result

The multiplayer server already provides a GSC `addtestclient()` function. It
creates an internal `NA_BOT` slot and the server generates user commands for it.
This is useful for testing server-side slot and command behavior without another
Steam copy. It is **not** a second real client, a client decoder, or a navigating
AI.

The installed-assets fixture now uses stock team/class callbacks and holds the
same healthy, equipped clients across one-second samples before quitting. The
three-bot probe passed locally under QEMU i386 with the existing licensed assets.
This is emulated server-side evidence, not native-host performance validation,
a real client decoder, or proof of multi-player synchronization.

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

The probe requires all admissions, every hold sample and clean shutdown to
pass. `--hold-seconds` defaults to 5; it samples at the start and every second
through the end. Missing or duplicate clients, a spectator/dead client, missing
class/weapon, script errors in the console log, and a nonzero exit all fail.

It uses a fresh temporary home/mod, symlinks installed assets read-only, binds
an ephemeral `127.0.0.1` port and never edits the game installation. The fixture
overrides registered `dm.gsc` and delegates initialization to stock `war::main`;
it is not a stock DM round. Only the fixture is distributed, not stock scripts.

The fixture identifies each `addtestclient()` result with the stock script field
`pers["isBot"]`, then sends `menuresponse` events for the allies team and the
`assault_mp,0` class choice. Stock callbacks set session state, select spawn
points and equip the class. The fixture never writes session state, spawns the
player directly, manufactures class stats, or changes authentication settings.

### Diagnosed failures

The previous fixture bypassed the team/class flow and omitted `pers["isBot"]`.
The installed stock `_globallogic.gsc` connection callback checks five class-stat
slots for non-bots and calls `kick` when they are zero. Internal test clients
have no persisted retail stats. Its explicit bot branch skips that player-stat
check. Private logging confirmed the kick at zero class stat 205 for all three bots.
The original fixture reproduced kicks; using the stock bot convention
and menu flow removes them without changing engine admission or kick policy.

Shutdown was an independent ownership defect. With `fs_game` nonempty,
`BG_ShutdownWeaponDefFiles` released weapon string references as though the
weapons had been loaded from raw files. Dedicated `BG_LoadWeaponDef` always
borrows fastfile definitions instead. The captured Debug stack reaches
`BG_FreeWeaponDefStrings` and `SL_FreeString` with a database-owned string,
triggering `!refStr->user`. Teardown now follows the loader's ownership choice.
The original failing fixture still kicks bots after this engine fix, but exits
cleanly. This isolates the engine repair from the fixture correction.

Asset-free regressions:

```sh
python3 scripts/tests/test_weapon_def_ownership.py --cxx clang++ --m32
python3 scripts/tests/test_weapon_def_ownership.py --cxx clang++ --sanitize
python3 scripts/tests/test_linux_bot_driver.py
```

The ownership test extracts production load, string release and shutdown bodies,
checking dedicated/client paths with and without a mod and fastfiles, shared
strings, every string-array boundary, repeated shutdown and 40 registration
cycles per build. Database/string services are checked doubles. The pre-fix
source fails on releasing a borrowed string. Driver tests reject incomplete,
duplicate, malformed and unhealthy hold evidence.

Death/respawn, deliberate disconnect/refill and map changes with bots remain
separate follow-up work. The independent lifecycle driver covers empty-server
restart/rotation; it does not establish bot survival through those transitions.
