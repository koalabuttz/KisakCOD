# Client packet/prediction integration — 2026-10-31

Local branch `dot/client-integration-2026-10-31` starts from client-parser checkpoint
`74d8eb94`. Server fixes stay independent. No push, PR, merge or deployment.

## Result

A new opt-in `KisakCOD-client-packet-test` links unchanged production client and
server code. The flow is cgame state publication -> command finishing -> packet
serialization -> Huffman compression -> client netchan encoding -> server netchan
decoding -> message/usercmd readers, with production archive storage and lookup.
`cl_cgame_mp.cpp` also joins the full-TU client compile milestone.

The harness verifies actual fields across that flow, including angles, weapon,
buttons, signed movement, origin and timestamps. A 300-packet run exercises all
three command/packet/prediction ring wrap boundaries. Additional cases cover the
32-command backlog cap, non-delta fallback when the snapshot is invalid, duplicate
prediction times, expired history and future-command errors. No new production
defect was established; no speculative behavior fix was made.

The harness supplies command scheduling and ring insertion instead of executing
`CL_CreateCmd` or cgame prediction. `Netchan_Transmit` captures encrypted payloads
and advances the outgoing sequence; it does not send sockets or implement netchan
fragmentation. Other doubles supply connection lookup, dvars, on-foot vehicle
status, diagnostic/error handlers, a checked fixed temporary arena, the Huffman
profiling clock, and the memory-fill intrinsic. Production `msgHuff` storage is
provided by the harness. These are test boundaries, not a runnable client host.

## Verification

Debug and RelWithDebInfo full client checkers: **176 checks, zero failures each**.
Each builds all four full client TUs and all three executables, verifies compile
flags, performs an incremental build, executes codec (25), parser/input (518), and
packet integration (15) runtime cases, checks default-OFF target absence, and
fingerprints the dedicated executable path before/after. The dedicated binary is
absent in this isolated worktree; this assertion guards against creating it.
Runtime code executes as ELF32 through the existing QEMU adapter, not native kernel
ELF32 execution. Logs are in `/workspace/kisakcod-client-integration-results/`.

Native execution remains unsupported by this environment. 32-bit ASan remains
unverified (the previously established runtime hangs before main; not retried).
No socket loop, real server session, graphical play or CoD4x session was run for
this milestone. Server lifecycle work belongs to the separate reliability branch.

## Next concrete integration boundary

`cl_main_mp.cpp` explicitly stops Linux compilation with `#error Steam Auth for
Arch` near its platform includes. `CL_CheckForResend` calls
`Steam_GetRawClientTicket`, base64 ticket encoding, and `Steam_GetClientSteamID64`
when generating the challenge request. A real headless handshake cannot honestly
be claimed by stubbing successful tickets. This is an unresolved implementation
dependency, not an access denial; no credential/security settings were changed.

`CL_Frame` additionally owns devgui, voice, userinfo, timeout, downloads, cgame time,
connection command creation and key authorization. Startup and incoming gamestate
paths bring in UI, assets and cgame services. A useful next bounded step is a
platform-neutral connection/transport seam that retains production handshake
serialization and consumes an explicit authentication-provider result, with
failure/retry/timeout tests. A supported Linux ticket provider and authorized live
server are required before calling that a working connection. Renderer/platform
initialization remains a separate large milestone; this change does not implement
it. Keep authentication failures explicit and preserve all protocol checks.

Independent read-only review found no blocking issues and reran the Debug packet
test successfully (15/15). Its suggestions were incorporated: the checker now
states that client transmission is reached with a transport double, and the
backlog case checks every decoded timestamp. The existing driver validator tests
also pass (8/8); `git diff --check` is clean.
