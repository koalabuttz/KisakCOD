# Client connection, engine networking and platform checkpoint — 2026-10-31

Branch `dot/client-lifecycle-2026-10-31` starts from `3da5d07e`, the prior headless
checkpoint with a whitespace-only amendment. The independent renderer-thread commit
is `cfe9369d`. Server reliability commits remain on their own branch and are not
merged here. All changes remain local: no push, PR, merge or deployment.

## Confirmed receive bugs and production changes

The first receive fixture linked the original production message readers,
command tokenizer, resend state machine and connectionless dispatcher. It failed
three of four cases: an unsolicited challenge advanced a client whose provider
was unavailable, a foreign challenge replaced the selected server, and a foreign
stats acknowledgement advanced the client to connected. Evidence: `baseline.log`.

Connection receive and timeout functions now live in
`cl_connection_receive_mp.cpp`, separated from client UI startup. A challenge
response can advance the client only after an authenticated request was accepted
by the transport API for the current session and exact selected endpoint. Failed
sends, provider loss on retry and cancellation invalidate that pending request;
the normal disconnect path resets it. Connect/stats/map notifications also check
the selected endpoint and the applicable state. Stats and challenge numeric fields
must be complete, bounded integers. No client ABI structures were enlarged.

Short OOB and PB prefixes are bounded before reading. Handshake text cannot silently
truncate into a valid command. End-of-datagram remains a valid command terminator:
the production server omits a NUL/newline when sending challenge responses. Large
binary master-server/PB packets retain their old routing. Independent review caught
an overly broad initial length check; that finding was fixed and covered with a
large delimiter-free master response and a large PB packet.

The 34-case receive fixture covers the reproduced failures, valid challenge/connect/
stats sequence, exact connect userinfo bytes, offline stats payload and 100ms retry,
cancel/retry/provider loss, timeout debounce, guarded-page short buffers and tokenizer
pool cleanup. Positive authentication is explicitly synthetic in this isolated
fixture. It has no socket send path. Transport, UI, voice, updater, map and storage
boundaries are doubles; it is not a game connection demonstration.

## Runnable discovery now uses engine networking

The diagnostic now calls real `NET_Init`, `NET_OutOfBandPrint`, `NET_GetClientPacket`,
`NET_Sleep` and `NET_Shutdown`. It also uses production dvar registration/shutdown
and Linux main-thread TLS/locking initialization. It prints
`network=engine-udp connected=false`, binds an ephemeral port, sends one read-only
`getinfo` to an explicitly selected endpoint, validates source/challenge and shares
a fixed monotonic deadline across every malformed reply. A full-size UDP receive
buffer prevents the engine's internal oversize-drain loop from extending that wait.

Host adapters supply diagnostics, clock, string/allocation ownership and absent
console/profiling hooks. No server or authentication exception is enabled. “Sent”
means the engine transport API accepted the query; a validated response is required
for discovery success. Linux provider status remains unavailable and successful
discovery exits 3, with `connected=false`.

This is an actual engine networking integration, but the diagnostic does **not**
call `CL_DispatchConnectionlessPacket` or `CL_Frame`. Those functions still need
UI/voice/updater/map/stats/game services. The receive fixture and runnable discovery
path are separate; adding the fixture's aborting game-service doubles to a shipping
host would not complete engine startup. No synthetic ticket option was added to
the executable.

## Platform and renderer-thread boundary

`cl_main_mp.cpp` now compiles in full on Linux. `CL_Setenv_f` uses POSIX
`setenv`/`unsetenv` with copied values, retaining Windows `_putenv` on Windows;
renaming `_putenv` to `putenv` would have retained a stack pointer. Its ten runtime
cases cover copying/lifetime, query/replacement, spacing, first equals, truncation
and the deletion branch. Audio includes in client-main and command code now use
the public API; `SND_InitDriver` is declared there. No OpenAL backend is needed for
these full-TU compile checks.

Commit `cfe9369d` separately fixes a Linux renderer-thread handoff that could
replace a pending frame before the renderer consumed it. The baseline reproduced
two failures; the fix preserves pending work and a real pthread consumes 64 frame
handoffs. Debug/release runtime checks pass, including pending-frame assertions.
This exercises actual thread synchronization, not graphics rendering, windows or
D3D drawing. Evidence: `render-runtime-baseline.log` and
`render-runtime-fixed-{debug,release}.log`.

## Final verification

Evidence directory: `/workspace/kisakcod-client-lifecycle-results/`.

- Debug and RelWithDebInfo full client checkers: **435/435 each**. Eight full client
  TUs compile; generated command checks also verify the diagnostic's actual engine
  sources. Each run includes incremental build, default-OFF target absence and
  dedicated-path fingerprint checks (absent before/after in this private tree).
- Runtime tests per configuration: codec 25, parser/input 518, packet/history 15,
  authentication/resend 20, receive lifecycle 34, platform 10, real UDP/event tests 8.
  The socket suite includes malformed floods, endpoint mismatch and deadline checks.
- Driver validator: 8/8. Independent review found no remaining blocking issue after
  the binary-response compatibility fix. Root independently reran receive 34/34
  and platform 10/10; evidence is in `*-independent.log`.
- Both engine-network diagnostics discovered a fresh real asset-backed `mp_crash`
  server with default authentication settings. Each exited 3 with
  `network=engine-udp`, `auth=unavailable`, `discovery=received discarded=0` and
  `connected=false`. Logs confirm an ephemeral engine UDP socket; server quit
  exited 0. Evidence: `engine-discovery/{report.json,debug.log,release.log,server.log}`.
  This establishes real discovery, not authentication or gameplay.
- Separate default-OFF dedicated builds pass in Debug and RelWithDebInfo. Their
  supported baseline asset-free server aggregates pass **12/12 each**. These are
  builds of this client branch, not an integration of the separate reliability fixes.

ELF32 runtime execution uses the established QEMU adapter; native ELF32 execution
is unavailable in this environment. The previously established 32-bit ASan startup
limitation was not retried. Windows build/runtime and graphical play are not verified.

## Remaining concrete dependencies

The repository has Steam headers and a Windows Steam DLL, but no Linux
`libsteam_api.so` was found in the authorized repository/assets/toolchain locations.
There is no verified Linux ticket provider. No login, credential, Steam environment
or authentication-policy changes were attempted. This is a missing implementation/
runtime dependency, not an access denial.

The next connection milestone needs a supported Linux ticket provider and an engine
host for the actual receive/connection frame lifecycle. The next graphics milestone
needs platform/window/backend initialization beyond the now-tested renderer-thread
handoff. Current discovery and synthetic lifecycle tests establish neither two-player
gameplay nor CoD4x compatibility. No requested operation has a denied or uncertain
execution outcome in this checkpoint.
