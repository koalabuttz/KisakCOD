# Real bot disconnect/refill and client discovery — 2026-10-31

Server branch `dot/bot-refill-2026-10-31` extends reviewed server checkpoint
`6797fa58` (`dot/bot-transitions-2026-10-31`). Client work is independent on
`dot/client-headless-2026-10-31`, extending `7483565f`; no server commits are
required by that series. No push, PR, merge, deployment or credential changes.

## Reproduced failures and fixes

The old release binary filled a three-slot server with three healthy bots, then
executed the fixture's real GSC kick path (`tempBanClient` -> `SV_KickClient` ->
`SV_DropClient`). All three disconnects and absent player entities were recorded.
After the ordinary zombie grace period, every refill admission failed despite
zero player entities. `SV_CheckTimeouts` incorrectly included zombie reclamation
inside the bot packet-timeout exemption, permanently consuming the slots.

Commit `8be223a4` moves expired-zombie reclamation outside that exemption while
preserving the normal grace period and keeping connected bots exempt from packet
silence. Extracted production tests fail three checks before this change and
pass afterward. They also preserve real-client six-check timeouts, expiry
boundaries and future-time clamping. Live baseline logs are under
`/workspace/kisakcod-refill-results/live-baseline*`.

Independent review identified a related initial-admission failure path:
`SV_AddTestClient` entered the world even when gamestate delivery had dropped the
client. Commit `e8116c7f` requires `CS_CLIENTLOADING` before world entry and returns
no entity on failure. A gamestate-failure double produces three failures against
old code; fixed code passes. This validates caller behavior, not an actual
oversized-gamestate serialization failure.

## Fixture behavior

`test_linux_bots.py --refill-cycles N --cycles M` performs real drop/refill rounds
before map transitions. Refill mode sets `sv_maxclients` to the bot count, forcing
actual reuse rather than filling previously unused slots. It records exact kick
intent, engine kick output, disappearance, new names in the same entity slots,
and a complete healthy hold for every cohort. The final cohort must survive all
subsequent map stages. Authentication remains at its default settings.

Stock teams permit only half of configured capacity on each side, so refill
mode sends normal menu responses to alternate allies/axis by entity number and
checks those exact teams. It disables bot firing for this lifecycle test to
avoid combat deaths between opposing teams. It does not set player session state,
spawn directly or manufacture stats. Default non-refill fixture behavior remains
unchanged. The first fixture attempt stopped before dropping anyone because it
requested all allies at full capacity; that attempt is not the slot-leak evidence.

## Validation and limits

Evidence: `/workspace/kisakcod-refill-results/`. Independent production and fixture
reviews found no blocking findings. The fixture validator passes 17 tests covering
slot/identity substitution, missing/duplicate/order errors, wrong teams, exact
kick windows and retention of the final cohort. Bot production-function tests
pass native host64 fatal ASan/UBSan and ELF32 without sanitizers through QEMU.
Final Debug/RelWithDebInfo asset-free aggregate checks and live results are below.

The previously documented native ELF32/32-bit sanitizer limitations remain.
Aggregate skips are explicit: `handshake_auth`, `server_test_clients`, `udp`.
The changed bot-function check is separately run in supported configurations.
No new access denial, pending approval or credential requirement arose. Real
retail/CoD4x client sessions and graphical gameplay are not established by bots or
read-only discovery. Windows full builds and real ticket-provider validation
remain outside this environment's evidence.

Final server results: Debug and RelWithDebInfo builds pass; final aggregate
**15/15 each**. Each live build passed **three full-capacity refill rounds plus
two map cycles**, with five-second holds sampled at 0–5 seconds each phase.
Debug completed in **128.5s**, release in **125.2s**. Each run admitted 12 bot
identities into the same three slots, observed nine planned kicks, retained the
last cohort through six map transitions, and quit cleanly with exit zero.
Authentication stayed 0/0. No assertion, fatal error or script error occurred.
Commit `25e32d0c` contains the independently reviewed live driver and usage.
These runs cover server-requested kick/drop, not client-originated disconnect
packets or real network-client synchronization.

Both Debug and release headless diagnostic binaries also completed read-only
`getinfo` discovery against a fresh real release server on loopback with installed
Crash assets. Each printed `auth=unavailable`, `discovery=received discarded=0`,
and `connected=false`, then returned the documented authentication-unavailable
status 3. The owned server quit normally with exit zero. Evidence is in
`/workspace/kisakcod-refill-results/client-discovery/report.json` and its logs.
This establishes a runnable socket/discovery path with an honest admission
boundary, not a real client connection or game loop.

## Applying the independent series

Server history is `f8bde51a` -> reliability checkpoint `fe4d1993` -> transition
checkpoint `6797fa58` -> `dot/bot-refill-2026-10-31`. Its cumulative patch is
`/workspace/kisakcod-refill-results/server-cumulative.patch`; the incremental
patch from `6797fa58` is `server-refill.patch` in that directory.

Client history is independently `f8bde51a` -> parser/input checkpoint `74d8eb94`
-> packet/prediction checkpoint `7483565f` -> `dot/client-headless-2026-10-31`.
Its checkpoint and cumulative/incremental patches are in
`/workspace/kisakcod-client-headless-results/`. Apply either series against its
stated base; do not stack the cumulative patch on its existing intermediate
commits. These are local review artifacts, not a merge or remote publication.

Next server work is a longer native soak and real network-client disconnect and
map synchronization coverage. Next client work is explicit platform initialization
and a supported ticket provider before authenticating a headless connection;
incoming gamestate and the engine event/frame loop remain separate integration
steps. Discovery deliberately stops at that boundary rather than simulating
successful credentials or admission.
