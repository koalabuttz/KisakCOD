# tests/linux_net — native test for `src/linux/net.cpp`

Test-only harness for the POSIX UDP network layer. **Nothing here is part of any
CMake target**: the build lists sources explicitly, so this directory is inert
unless you run the script below.

## What it proves

`test_net.cpp` drives the real `src/linux/net.cpp` against real kernel sockets
on loopback, as a 32-bit binary:

| Area | Cases |
| --- | --- |
| Layout | `netadr_t` is 20 bytes, `msg_t` is 40 bytes, field offsets |
| Payload | 256-byte binary payload round-trip (all byte values incl. `0x00`/`0xFF`) |
| Byte order | `netadr_t.port` stays network order (`== htons(peer port)`), `ip[4]` octets in order |
| Non-blocking | empty queue returns `qfalse` without blocking, leaves `cursize` untouched |
| Oversize | 300-byte datagram into a 32-byte buffer is rejected, guard bytes untouched, socket drained |
| Zero length | zero-byte datagram accepted with `cursize == 0` |
| Send | `Sys_SendPacket` delivers the exact bytes and reports success |
| Parsing | `127.0.0.1`, `localhost`, `255.255.255.255`, legacy `127.1`, unknown name, empty string |
| LAN | loopback/bot, 10/8, 127/8, 169.254/16, 172.16–31/12, 192.168/16 true; 172.32, 8.8.8.8, 203.0.113 false |
| Sleep | `NET_Sleep(0)` immediate, ~120 ms idle sleep, early wake on readable socket |
| Port fallback | occupied `net_port` advances within 10 ports, cvar updated, engine receives on the fallback |
| Rebind | 5 latched port changes, no descriptor leaks, shutdown stops reads, init/shutdown idempotent |
| `net_ip` | binding a specific interface address ignores `127.0.0.1` and receives on the bound address |
| `net_noudp` | disables the socket, and clearing it restores the socket on `net_restart` |
| SOCKS | `net_socksEnabled` produces one explicit "not supported" warning, direct UDP still works |
| TCP helper | `NET_TCPIPSocket` bind/connect; failure sentinel is `-1`, never `0` |
| `sv_loopbackTestAuth` | the local-auth test guard: see the section below (`test_loopback_auth.cpp`) |

The engine services the layer calls (`Com_Printf`, `Dvar_Register*`,
`Dvar_FindVar`, `I_stricmp`, `I_strncpyz`, `NET_AdrToString`) are provided by
`shim_engine.cpp` behind the minimal stand-ins in `shim/`. The stand-in types are
byte-identical to the engine headers and are asserted in the test, so the results
reflect the real ABI.

## Loopback local-auth test guard (`sv_loopbackTestAuth`)

`test_loopback_auth.cpp` covers `NET_InitLoopbackAuthTest()`,
`NET_LoopbackAuthTestEnabled()`, `NET_CanBypassLoopbackAuth()` and the
configuration gate in front of every bind (`NET_CheckLoopbackTestConfig`).

Two rules shape how it is written:

**A start-up decision needs its own process.** The guard captures the cvar once
per process and `NET_Shutdown()` deliberately keeps that answer, so nothing
inside one process can produce a second, different decision. Every start-up case
therefore runs in a forked child that is created *before* the parent's first
`NET_Init()`, reports its check counts back over a pipe, and exits with its own
status. `RunLoopbackAuthScenarioProcesses()` prints one line per scenario.

**"Refused" means "no socket", not "an error was printed".** `Com_Error()` in
this harness logs and returns where the engine aborts, so the diagnostic alone
proves nothing. Each negative case therefore also asserts that the port is free
(a wildcard `bind()` from the test succeeds, which only happens while nothing
holds the port) and that a datagram sent to `127.0.0.1:<port>` is not read back.
Positive cases assert the opposite through a real UDP round-trip, and check that
the engine's socket is bound to `127.0.0.1` *specifically* rather than to every
interface — a wildcard holder would exclude the test's own `127.0.0.2:<port>`
bind, while a socket on `127.0.0.1` leaves it free. That probe is self-tested
(`LoopbackProbeIsMeaningful()`), and skipped loudly on a kernel where it cannot
discriminate.

Scenarios, one process each:

| Scenario | What it pins down |
| --- | --- |
| `valid-startup` | enabled + `net_ip 127.0.0.1` + `dedicated 1`: captured answer on, `127.0.0.1` may bypass, socket really bound and receiving, bind is loopback-specific |
| `predicate` | per packet: `127.0.0.1` over `NA_IP` with any source port passes; `127.0.0.2`, `127.0.0.10`, `127.1.0.1`, LAN, link-local, public, `0.0.0.0`, broadcast, multicast, and the `NA_LOOPBACK`/`NA_BOT`/`NA_BAD`/`NA_BROADCAST`/`NA_IPX` pseudo-types are refused |
| `net-noudp` | enabled and safe but `net_noudp 1`: the capture stays on, no socket exists, no bypass, no diagnostic |
| `shutdown-reinit` | `NET_Shutdown()` keeps the captured answer but closes the gate; re-init does not re-read the cvar; restoring the start-up value restores the bypass |
| `runtime-revoke` | an engine-side `Dvar_Set` that turns the cvar off revokes at once (verified while the socket is still up and loopback-bound), `net_restart` does not resurrect it, restoring the value restores the bypass |
| `net-ip-veto` | `net_ip` changed at runtime (`localhost`, empty, `0.0.0.0`, `127.0.0.2`, LAN) vetoes while the socket is untouched and still loopback; `dedicated` 0 and a NULL `com_dedicated` veto; `dedicated` 1/2 restore |
| `restart-safe` | `net_restart` on a safe configuration re-binds the same loopback port and keeps the bypass; `net_noudp` through `net_restart` closes the socket and the gate, and clearing it brings both back |
| `pending-latched` | a latched-but-unapplied `net_ip` neither revokes nor widens anything; applying it vetoes before any rebind; the rebind it asks for is refused before an unsafe socket exists |
| `unsafe-restart-matrix` | empty, `localhost`, `0.0.0.0`, `127.0.0.2`, `127.1.0.1`, `127.0.0.1 ` (trailing space), the host's LAN address and its host name are each refused with a diagnostic, no bypass, no socket, nothing received — repeated with `net_noudp 1` |
| `unsafe-startup-empty`, `unsafe-startup-localhost`, `unsafe-startup-wildcard` | the same refusals on the very first bind of a process |
| `dedicated-zero`, `dedicated-unset` | enabled and loopback but a listen server, or a process with no `com_dedicated`: refused, nothing bound |

The parent process itself never enables the switch, so it also asserts the
default case in place: the cvar defaults to off, carries `DVAR_INIT` (`0x10`),
the captured answer is disabled, and a `127.0.0.1` client is refused even when
the server is dedicated, `net_ip` is exactly `127.0.0.1` and the socket is
provably bound to it. Turning the cvar on afterwards, or restarting, or a full
`NET_Shutdown()`/`NET_Init()` cycle, still grants nothing.

### Shim surface this uses, and what it does not model

`shim/universal/q_shared.h` mirrors `DVAR_INIT = 0x10` (copied from
`src/universal/q_shared.h`). `shim/qcommon/qcommon.h` mirrors
`extern const dvar_t *com_dedicated;` (as in `src/qcommon/qcommon.h`) and
declares `Dvar_FindVar()`, which `shim_engine.cpp` forwards to the same lookup
the shim already had. `com_dedicated` is linked whenever a dvar named
`dedicated` is registered or seeded, and `Shim_ClearComDedicated()` drops the
pointer. `Shim_DvarSeedBool()` adds boolean seeding; seeding stands in for
`+set <dvar> <value>` on the command line, because registration keeps the value
that is already there.

What this cannot show, and the full engine binary has to:

- the real `+set` path (`DVAR_EXTERNAL`/user-created dvar being converted when
  the dvar is registered) — the shim models it as pre-seeding a value, not as a
  string-to-bool conversion;
- real dvar latching: `pending-latched` writes `latched.string` by hand to model
  a queued `net_ip` change, it does not exercise the engine's `Dvar_Set*`
  machinery;
- `Com_Error(ERR_FATAL)` aborting. Every "refused" case here asserts socket
  state instead, which is strictly more than the diagnostic and does not depend
  on it, but a case that only aborts *after* opening a socket would be caught
  only if it leaves the socket open — the post-bind mismatch branch of
  `NET_IPSocket()` cannot be provoked from the public surface at all (it needs
  an address that binds somewhere other than configured, e.g. a doctored
  resolver);
- the real handshake (`server_mp`) refusing or accepting a client. This covers
  the guard's answer, not its use.

One production behaviour worth knowing, found while writing these cases: after a
refused bind the layer stays "networking enabled" with no socket and keeps its
previous `lastNetIP`/`lastNetPort`, so an identical `NET_Restart()` is a no-op
rather than a retry. Behind a fatal `Com_Error` that is unreachable in
production, and it fails closed, so the matrix re-binds with a fresh port instead
of asserting recovery.

## Running

```sh
sh tests/linux_net/run_tests.sh
```

Requires a multilib toolchain: `clang++ -m32` must link (`gcc-multilib`).
The script exits non-zero if any check fails. The binary runs the loopback-auth
scenarios first (each in a forked child) and then the in-process suite; the final
line counts both.
