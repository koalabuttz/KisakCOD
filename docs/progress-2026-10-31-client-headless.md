# Linux client discovery/authentication boundary — 2026-10-31

Branch `dot/client-headless-2026-10-31`, based on `7483565f`, independently follows
the client parser and packet integration branches. Server reliability changes
remain separate. All work is local; no push, PR, merge or deployment.

## Runnable result

`KisakCOD-client-headless` starts a real Linux UDP socket and event wait, sends one
read-only `getinfo` request to an explicitly selected IPv4 endpoint, validates
source and challenge, and reports the outcome. It has no default remote target,
no game admission path and no synthetic authentication option. Socket lifetime is
owned by the process scope and all waits share a fixed monotonic deadline.

The diagnostic shares the production authentication boundary, which reports
`unavailable` on Linux. Successful discovery exits 3 and prints `connected=false`;
it does not claim a successful authenticated connection. The diagnostic's POSIX
socket/event startup is independent of engine `NET_Init` and `CL_Frame`. It does
not reuse the engine netchan, packet receive dispatcher, or rendering/game host.

Example (native ELF32-capable Linux):

```sh
build-linux/client-core/bin/KisakCOD-client-headless --server 127.0.0.1:28960 --timeout-ms 3000
```

## Production boundary

`cl_connection_mp.cpp` now owns the existing `CL_CheckForResend` and stat packet
priority helper, extracted from `cl_main_mp.cpp`. Retry timing, attempt counters,
CHALLENGING/connect serialization and SENDINGSTATS behavior are preserved. The
CONNECTING branch calls `CL_GetPlatformChallenge` and sends nothing when it fails.
The diagnostic never sends that challenge, even if a provider is later added.

`cl_auth.cpp` retains the Windows Steam ticket borrow/cancellation calls and exact
`getchallenge 0 "<base64>" "<SteamID64>"` format. It neither generates nor retains
tickets. Ticket bytes are bounded to the existing Windows 1024-byte ticket capacity
before encoding; the former roundtrip length validation is retained and also
compares bytes. Missing identity, invalid tickets and insufficient output capacity
fail closed. Linux has no successful ticket provider yet. The old Linux compile
#error is replaced by that explicit unavailable result; no authentication checks
or server admission checks are bypassed.

## Verification

Final Debug and RelWithDebInfo client aggregate checks each pass **251/251**, including the prior
codec, parser/input and packet/history tests. Each also builds the two new full
client TUs, checks production compiler flags, validates default-OFF target absence,
and ensures the private worktree's dedicated executable path is unchanged
(absent before/after). Runtime evidence:

- Codec: 25 checks; parser/input: 518; packet/history: 15.
- Authentication/resend: 20 checks. This includes exact request format, binary
  ticket bounds/padding, output capacity, real Linux provider unavailable status,
  real production resend deadlines/counts, demo suppression and no challenge send.
- Real loopback UDP/event tests: 8. They cover valid discovery, malformed/wrong
  challenge/duplicate/NUL/oversized replies, wrong endpoint, silent timeout, no
  default target, invalid arguments, and sustained malformed traffic that cannot
  extend the deadline. Test peers use ephemeral loopback ports.
- Driver validator: 8 tests. Independent read-only review found no blocking issue;
  its sustained-flood suggestion is incorporated.

Root coordinator additionally ran both diagnostic binaries against an isolated
real asset-backed server on `mp_crash`, with default authentication settings.
Both exited 3 with `auth=unavailable`, `discovery=received discarded=0` and
`connected=false`. The dedicated server exited 0 after quit. This validates actual
server discovery only, not authentication, gameplay or two-player compatibility.
Evidence is copied to `/workspace/kisakcod-client-headless-results/live-discovery/`;
full aggregate logs are in the parent results directory.

## Limits and next step

Runtime ELF32 tests use the existing QEMU adapter because this environment cannot
execute ELF32 natively. The existing 32-bit ASan startup limitation was not retried.
Windows provider/runtime and full Windows client build are **not verified** here.
Auth/resend tests use controlled host state and transport doubles; only the separate
discovery diagnostic uses real sockets. No renderer, cgame simulation, actual
connection acceptance or playable Linux client is delivered.

A full `cl_main_mp.cpp` compile probe initially needs OpenAL/Tracy headers. With
existing authorized headers supplied, it reaches `_putenv` as an unresolved Linux
platform call. That call's Windows copy semantics must be preserved when porting
it; simply renaming it to POSIX `putenv` would retain a stack pointer. This probe is
not part of the portable milestone and did not change that function.

Next: integrate a supported Linux ticket provider and a real engine connection
host/receive loop with explicit failure/retry states. Keep the standalone discovery
result separate from game connection success. Full startup still depends on host
services, audio/rendering boundaries and assets. No action was denied or left with
an uncertain execution outcome in this milestone; these are known implementation
and runtime-environment limits.
