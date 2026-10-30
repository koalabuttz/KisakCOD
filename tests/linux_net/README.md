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

The engine services the layer calls (`Com_Printf`, `Dvar_Register*`, `I_stricmp`,
`I_strncpyz`, `NET_AdrToString`) are provided by `shim_engine.cpp` behind the
minimal stand-ins in `shim/`. The stand-in types are byte-identical to the engine
headers and are asserted in the test, so the results reflect the real ABI.

## Running

```sh
sh tests/linux_net/run_tests.sh
```

Requires a multilib toolchain: `clang++ -m32` must link (`gcc-multilib`).
The script exits non-zero if any check fails.
