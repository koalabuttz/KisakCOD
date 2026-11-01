# Connected-bot transitions and database publication — 2026-10-31

Local branch `dot/bot-transitions-2026-10-31` starts from server checkpoint
`fe4d1993`. No push, PR, merge, deployment or authentication changes.

## Production fixes

`acb72163` completes retained internal bots' map-load transition. Full map loads
leave their slots in `CS_CONNECTED`; unlike network clients they never send the
packets that request gamestate and enter the world. `SV_UpdateBots` now sends
those internal clients a gamestate, enters only if it succeeded, and moves only
active bots. Ordinary clients and failed/dropped slots are not re-entered.
A live baseline retained slots but produced zero player entities after restart.
The extended production-function regression fails seven checks on old code.

`43250a3c` removes premature database notifications inside `DB_LoadXZone`'s
batch-building loop. Only the final complete batch now wakes the worker. Ordinary
release startup under QEMU reproduced `!g_loadingAssets` before any bot ran.
An immediate-consumer regression fails old code with "consumer saw incomplete
queue count" and passes the fixed producer with immediate/deferred consumers,
repeated batches, empty batches and preserved error assertions. This tests the
publication contract, not native concurrent scheduling. Event implementations
were independently reviewed: the wake helpers reset completion events, and
`Sys_NotifyDatabase` signals the worker.

## Live fixture

`test_linux_bots.py --cycles N` adds repeated restart, change to Shipment and
rotation back to the requested map. It creates bots only on the initial map and
requires the same entity/name pairs, healthy playing state, team, class and
weapon at every hold sample. Each stage has separate map/authentication markers.
The fixture wraps the stock connect callback to restore the stock `pers["isBot"]`
marker only for its recorded identities before invoking the original callback.
Restoring it after a one-second wait is too late: the stock stat check can already
have queued a kick. This is fixture setup, not an engine authentication bypass or
proof that arbitrary mods preserve bot state automatically.

## Verification and limits

Evidence directory: `/workspace/kisakcod-transition-results/`.
Debug and RelWithDebInfo server builds pass. Both compatible aggregate runs pass
15/15 checks. The known incompatible checks are explicitly skipped:
`handshake_auth` and `server_test_clients` require the unavailable 32-bit sanitizer
runtime; `udp` launches ELF32 directly from shell on a kernel that rejects it.
Supplemental bot function tests pass native host64 fatal ASan/UBSan and ELF32
without sanitizers under QEMU. Runner unit tests pass 24/24. Prior checkpoint
contains supplemental handshake/UDP results; those are not new runs here.

Independent review found no actionable production issues and reran database and
bot host64 sanitizer tests. Native ELF32 execution and native 32-bit ASan remain
unverified; no restrictions were bypassed. Live runs use QEMU and existing local
game assets, bind only loopback and retain default authentication dvars. Bots do
not establish real client snapshot decoding, two-player gameplay or CoD4x
compatibility. No Windows run was available.

Final live repetition and fixture review results are recorded below.

Final connected-bot runs passed in **Debug (84.3s)** and **RelWithDebInfo (81.0s)**:
three bots, two cycles each, seven map stages, five-second holds sampled at
0–5 seconds per stage, then clean quit. Every stage preserved entity/name pairs,
positive health, playing state, allies/CLASS_ASSAULT and an equipped weapon;
default authentication stayed 0/0. No assertions, script errors or pre-quit kicks.
An additional release one-cycle/two-second-hold run passed (34.7s).
These ordinary runs did not reproduce `!g_loadingAssets` after the fix; finite
runs alone do not prove absence of races.

Final aggregate reruns passed **15/15 each** after the fixture changes. Fixture
unit tests passed **11/11**, including dropped/duplicated evidence, stale stage,
wrong map/authentication, replacement and identity changes. Independent fixture
review found no actionable issues and reran those tests. Commit `169d9ced` holds
the fixture and usage documentation.

The parallel client branch `dot/client-integration-2026-10-31`, commit `7483565f`,
adds unchanged production command/packet/prediction integration. Debug/release
client checkers pass 176/176 each. Its checkpoint documents transport doubles and
the actual Linux authentication/startup dependencies before a headless connection
can be claimed. No socket or native gameplay claim is made.

## Next work

Server: extend the real lifecycle fixture through explicit bot disconnect and
refill before another map cycle, then run a longer native soak on a machine that
supports the shipped ELF32 build and sanitizers. Preserve exact admission/drop
accounting; the current tests intentionally do not claim real `SV_DropClient`
cleanup or refill coverage. Run real retail/CoD4x clients before drawing client
compatibility conclusions.

Client: implement the bounded connection/authentication-provider seam described
in the client integration checkpoint, keeping failure/retry behavior explicit.
A real ticket provider, startup services and incoming gamestate integration are
prerequisites to a headless socket session; graphical renderer/platform work is
still a separate milestone. No access approval was denied or is pending in this
checkpoint. The native execution limitations are environmental; the client
connection blockers are unresolved implementations.

Final release empty-server lifecycle also passed: three restart/change/rotation
cycles with fresh getinfo/getstatus and console status, then clean console quit;
a separate initial-map run passed SIGTERM shutdown. Both exited zero and recorded
game cleanup. Same-map resource growth after warm-up was at most 4 KiB RSS,
zero descriptors and zero threads. Reports live under
`/workspace/kisakcod-transition-results/lifecycle-release/`; this bounded emulated
run is not proof of native leak freedom.
