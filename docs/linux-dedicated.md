# Native Linux dedicated server

The native Linux target is a **32-bit x86 multiplayer dedicated server**. It
produces an ELF executable, not a Windows executable, and does not use Wine.
The Windows MP/SP/editor build paths remain separate.

This is an initial port, not a gameplay-ready release. There is no native Linux
graphical client. In particular, **Steam authentication is not implemented on
Linux yet: ordinary client tickets are rejected**. An explicit startup-only,
loopback-only test mode can skip ticket verification; it does not by itself
establish playable-client compatibility. Separately, the opt-in boolean
`sv_allowProtocol6` lets retail CoD4 1.7 clients through the connect-version check
(they advertise protocol 6). In that local test mode one stock 1.7 client has
joined and played briefly; see
[Retail 1.7 clients and `sv_allowProtocol6`](#retail-17-clients-and-sv_allowprotocol6).
One short single-player session is a connectivity result, not a compatibility
guarantee. Do not deploy this build as a public game server or treat the
bootstrap check as a multiplayer test.

## Build

On a Debian/Ubuntu x86-64 host, install the native tools and 32-bit development
libraries:

```sh
sudo apt-get update
sudo apt-get install cmake ninja-build clang g++-multilib
bash build-linux.sh
```

The script defaults to `RelWithDebInfo`, builds only `KisakCOD-dedi`, and writes
`bin/KisakCOD-dedi`. Configuration fetches the pinned Tracy dependency and needs
network access on its first run. Clang is required for the Microsoft C++ language
extensions still used by the shared engine code. The compiler and linker both
use `-m32`; 64-bit asset/VM layouts are not supported. The Linux code requires
SSE2 and a little-endian x86 host.

Optional arguments and parallelism:

```sh
bash build-linux.sh build-linux Debug
CMAKE_BUILD_PARALLEL_LEVEL=4 bash build-linux.sh build-linux/release RelWithDebInfo
```

Use separate build directories for different configurations. The output filename
is shared unless you set `CMAKE_RUNTIME_OUTPUT_DIRECTORY` when configuring:

```sh
cmake -S . -B build-linux/debug -G Ninja \
  -DKISAK_PLATFORM=linux -DCMAKE_BUILD_TYPE=Debug \
  -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++ \
  -DCMAKE_EXPORT_COMPILE_COMMANDS=ON \
  -DCMAKE_RUNTIME_OUTPUT_DIRECTORY="$PWD/build-linux/debug/bin"
cmake --build build-linux/debug --target KisakCOD-dedi --parallel 2
```

For editor diagnostics, point clangd at the configured compilation database,
for example `clangd --compile-commands-dir=build-linux`. Without it the language
server may report missing engine headers even though the CMake build succeeds.

Do not set `WIN32` or `_WIN32` for the native build. Setting `KISAK_PLATFORM=mingw`
and using `build-linux-for-windows.sh` still selects the unrelated Windows
cross-build.

## Startup and assets

```sh
./bin/KisakCOD-dedi --help
./bin/KisakCOD-dedi --version
./bin/KisakCOD-dedi +set fs_basepath /path/to/cod4 \
  +set fs_homepath /path/to/server-home +set net_ip 127.0.0.1
```

Gameplay requires legally obtained COD4 data, including the small configuration
and localization files as well as the MP fastfiles. Keep the original directory
layout. The native server uses case-sensitive filesystem lookups; it translates
engine path separators but does not case-fold filenames. Raw source-asset mode
(`useFastFile 0`) is explicitly rejected by this target.

Without assets, the expected result is a nonzero exit during real engine
initialization with a message such as:

```text
Couldn't load fileSysCheck.cfg. Make sure Call of Duty is run from the correct folder.
```

That result verifies the executable can enter `Com_Init` and filesystem startup.
It does **not** verify fastfile deserialization, a loaded map, server frames,
client connections, or network compatibility with an existing client.

Console commands come from stdin; SIGINT/SIGTERM request shutdown on the main
thread. Fatal initialization errors print to the terminal and exit nonzero,
rather than displaying a Windows dialog.

## Asset-free checks

Run the bootstrap check against a compiled native executable:

```sh
python3 scripts/tests/test_linux_bootstrap.py
# Or supply another build's executable:
python3 scripts/tests/test_linux_bootstrap.py build-linux/debug/bin/KisakCOD-dedi
```

Run the complete asset-free set against an already configured/built target:

```sh
python3 scripts/tests/run_linux_checks.py \
  --build-dir build-linux --binary bin/KisakCOD-dedi
```

The runner prints the exact commands and fails on missing artifacts, unsuitable
compile databases, nonzero results, or timeouts. `--list` prints the command list
without executing it; `--log-dir` retains text logs. It builds test harnesses, not
the server itself. The Linux GitHub Actions workflow runs this entrypoint for
Debug and RelWithDebInfo on Ubuntu 24.04, uploading only the native executable
and check logs. It never downloads or uploads licensed game data. Hosted CI
results must be checked separately from local runs; existing Windows CI remains
responsible for MSVC builds.

Runtime and UDP checks can also be run individually. They need the same multilib
toolchain, but no game assets:

```sh
python3 scripts/tests/test_linux_runtime.py
python3 scripts/tests/test_linux_fastfile_io.py
python3 scripts/tests/test_linux_debugger.py
bash tests/linux_net/run_tests.sh
```

The runtime suite exercises event/reset semantics, cooperative thread suspension,
thread-local storage, reserved/committed memory, symlink-safe directory removal,
command-line bounds, and batched console input. Its isolated engine dependencies
are test doubles; it is not a complete server simulation.

Additional native file/virtual-memory checks can be built directly:

```sh
clang++ -m32 -std=c++20 -fms-extensions -pthread -Isrc \
  -DSYS_FILEIO_SELFTEST src/linux/sys_fileio.cpp -o build-linux/fileio-check
./build-linux/fileio-check
clang++ -m32 -std=c++20 -fms-extensions -pthread -Isrc \
  -DKISAK_SYS_VIRTUALMEM_SELFTEST src/linux/sys_virtualmem.cpp \
  src/linux/sys_fileio.cpp -o build-linux/virtualmem-check
./build-linux/virtualmem-check
```

See also [the shared regression checks](../scripts/tests/README.md).

## Validation recorded for this port

On this Linux x86-64 host, CMake 3.31.6, Ninja 1.12.1 and Debian Clang 19.1.7
built both Debug and RelWithDebInfo 32-bit executables. Both passed the asset-free
bootstrap checks: ELF32/i386 format, help/version, command-line bounds, native log
paths, and real engine/filesystem initialization ending at the expected missing
`fileSysCheck.cfg` error. The native runtime, UDP, file-I/O, virtual-memory,
fastfile short-read and debugger boundary checks also passed, alongside the
selected shared regression suites. These checks use no licensed game assets.

## Optional check with installed game assets

The server can use an existing Windows COD4 Steam installation on Linux. The
startup check stages symlinks to its assets and uses a separate temporary writable
home; it disables UDP and does not copy or modify the installed game files:

```sh
python3 scripts/tests/test_linux_asset_startup.py \
  "$HOME/.steam/debian-installation/steamapps/common/Call of Duty 4"
```

On this machine, that check loads and unloads `code_post_gfx_mp`,
`localized_code_post_gfx_mp`, `common_mp`, and `localized_common_mp`, completes
common initialization, and exits normally through `quit`. Their installed
fastfile headers are `IWffu100`, version 5.

To extend the check through map initialization, server frames, `status`, and
clean shutdown:

```sh
python3 scripts/tests/test_linux_asset_startup.py \
  "$HOME/.steam/debian-installation/steamapps/common/Call of Duty 4" \
  --map mp_crash
```

`mp_crash` and `mp_shipment` have completed this check in Debug with the installed
retail assets; `mp_crash` also passes in RelWithDebInfo. The first map test exposed
an EOF loop in the script scanner: exhausted input was restarted when no local
client existed. The scanner now dispatches EOF independently of
client count, with an asset-free SP/MP regression covering zero and one local
client. This fixes map initialization; it does not establish real-client or
multiplayer gameplay compatibility.

## Optional lifecycle soak and UDP query checks

For repeated map restart/change/rotation with real loopback UDP queries:

```sh
python3 scripts/tests/test_linux_server_lifecycle.py \
  "$HOME/.steam/debian-installation/steamapps/common/Call of Duty 4" \
  --binary bin/KisakCOD-dedi --output-dir build-linux/lifecycle-run \
  --cycles 5 --hold-seconds 15 --shutdown both
```

Use a new output directory for each run. This test stages the same isolated asset
view and writable home, binds only `127.0.0.1` on a dynamically selected port,
and never authenticates or connects a player. Each cycle performs `map_restart`,
changes to `mp_shipment`, and uses `map_rotate` to return to `mp_crash`. Fresh
challenge-correlated `getinfo`/`getstatus` replies and console `status` acknowledge
each stage. A separate process checks SIGTERM shutdown in addition to the main
run's console `quit`.

Logs and JSON reports retain RSS, virtual size, CPU time, descriptor counts and
thread counts sampled from `/proc`. Resource checks compare `mp_crash` at the
end of each full cycle with the first completed cycle as warm-up. Defaults allow
64 MiB RSS growth, two additional descriptors and no extra threads; stricter
limits can be selected with `--max-rss-growth-mib` and `--max-fd-growth`. These
are regression thresholds, not a proof that the process cannot leak. One cycle
alone has no post-warm-up resource comparison. Mid-load peaks are recorded but
not compared to steady-map thresholds. CPU and virtual size are measurements,
not enforced budgets.

A five-cycle run passed locally in both Debug and RelWithDebInfo, with 15-second
holds at each stage, fresh replies throughout, and both shutdown paths exiting
zero. Same-map RSS growth after warm-up was at most 100 KiB, with no additional
descriptors or threads. Those first runs also exposed an idle-CPU issue in the
network wait's deadline rounding: a requested 1 ms wait usually rounded down to
zero and polled instead of sleeping. Positive fractional deadlines now round up.
After rebuilding, both configurations passed another three cycles with
10-second holds; same-map RSS grew at most 8 KiB, with no descriptor/thread
growth. Median CPU over steady one-second samples fell from about 101% of one
core to 10% in Debug and 8% in RelWithDebInfo on this host. Loading spikes and
machine scheduling affect these numbers; they are observations, not budgets.

The driver's own asset-free parser, shutdown-evidence and error-cleanup tests
can run in CI without starting the engine:

```sh
python3 scripts/tests/test_linux_lifecycle_driver.py
```

## Local connection testing without Steam authentication

For authorized testing on the same machine only, add these startup settings to
an otherwise configured server launch:

```sh
bin/KisakCOD-dedi \
  +set fs_basepath /path/to/isolated/assets \
  +set fs_homepath /path/to/separate/writable/home \
  +set dedicated 1 +set net_ip 127.0.0.1 +set net_port 28960 \
  +set sv_loopbackTestAuth 1 +map mp_crash
```

This option is **off by default**, unarchived, and captured once at process
startup. Console/script setters cannot enable it; `net_restart` and map changes
do not change the captured choice. Restart the process without the flag (or
with `+set sv_loopbackTestAuth 0`) to restore ordinary fail-closed ticket checks.
That rollback does not implement Linux Steam authentication.

The test mode requires all of the following before skipping a ticket check:

- A dedicated server with literal `net_ip 127.0.0.1`. Omitted/empty settings,
  `localhost`, wildcard, LAN, hostname and other loopback addresses are refused
  at startup, even with UDP disabled. This engine treats `localhost` as a
  wildcard bind, not as a safe alternative.
- A live IPv4 UDP socket actually bound to `127.0.0.1`, verified with the kernel,
  and a current option/configuration that still agrees with the startup choice.
- A UDP source address exactly `127.0.0.1`, not the wider `127/8` range, LAN
  addresses, or internal loopback/bot address types.

It generates `local-test-…` session identities, **not authenticated Steam IDs**.
They are not persistent account identities. Protocol, source-address/challenge
matching, passwords and capacity checks remain in place. Local processes are
trusted: a local proxy or port forward can relay remote traffic, so do not
forward the port or use this mode for public/LAN hosting.

A retail CoD4 1.7 client also needs `sv_allowProtocol6 1` in that launch, because
it advertises connect protocol 6; see
[Retail 1.7 clients and `sv_allowProtocol6`](#retail-17-clients-and-sv_allowprotocol6).

Without opt-in, missing custom ticket/ID arguments now receive an explicit
handshake error rather than asserting or silently retrying. Malformed fields
are rejected before Base64 decoding, and a well-formed native request reports
that Steam authentication is unavailable. Authentication tickets are not logged.

### Client compatibility is a separate boundary

This codebase serves connect protocol **1**, and `getinfo`/`getstatus` still
advertise it. Changing that number to impersonate a stock or CoD4x server would
not help: a matching integer alone cannot fix challenge-response, gamestate,
snapshot or gameplay serialization differences. The
[CoD4x server source](https://github.com/callofduty4x/CoD4x_Server/blob/master/src/sv_client.c)
has a different challenge-response format, including an echoed client challenge
and protocol information, so it does not complete a normal playable protocol-1
channel with this build.

A version mismatch is answered with a client-visible `error` packet naming the
required and received versions. It was originally a bare
`EXE_SERVER_IS_DIFFERENT_VER` packet, which clients ignore, so a mismatched
client kept retrying while displaying "awaiting challenge."

#### Retail 1.7 clients and `sv_allowProtocol6`

An installed retail client (`iw3mp.exe`, "CoD4 MP 1.7", CoD4x loader disabled)
advertises connect protocol **6**. Examining that client's gamestate parser,
constant configstring table, and entity/player-state netfield tables showed them
to be identical to this 1.0 code; emulating its entity decoder against a real
server gamestate decoded all 70 baselines. The examined encodings match; other
1.7 differences, such as gameplay or script behaviour, have not been audited.

`sv_allowProtocol6` is an **opt-in boolean that defaults to off** (registered in
`SV_Init` with `DVAR_NOFLAG`). When it is enabled, a `connect` advertising
protocol 6 is handled as protocol 1; every other number is still rejected, and
the error text then names both accepted versions:

```text
error
This server requires protocol version 1 (CoD4 1.0) or 6 (CoD4 1.7); your client uses protocol 21.
```

With the dvar off, the same reply is
`error` / `This server requires protocol version 1 (CoD4 1.0); your client uses
protocol 21.` The switch changes the version check only: the challenge token, the
Steam ticket handshake, the game password and the capacity checks are unchanged.
Because native Steam ticket verification is still unimplemented, a retail client
can only reach a real server through the loopback test mode:

```sh
./bin/KisakCOD-dedi +set fs_basepath /path/to/cod4 \
  +set fs_homepath /path/to/server-home +set net_ip 127.0.0.1 \
  +set dedicated 1 +set sv_loopbackTestAuth 1 +set sv_allowProtocol6 1 \
  +map mp_crash
```

Windows clients previously desynchronised against this server for an unrelated
reason: the message Huffman tree was built with a platform-dependent tie order,
which swapped the codes for bytes 155 and 205. That is fixed in `23a62702`
("Build the message Huffman tree identically on every platform"). Before that
fix, any Windows client mis-decoded messages containing either byte, whatever
protocol it sent.

Verified: with that fix plus `sv_allowProtocol6 1` and `sv_loopbackTestAuth 1`,
one stock retail 1.7 client connected to `mp_crash` and played briefly with one
player; no obvious problem was observed. Connection admission for protocol 6 is
covered by the checks described below.

Not verified:
- more than one player, or a 1.7 client mixed with 1.0 clients.
- map changes and rotation while a 1.7 client is connected.
- long sessions, or any load beyond a brief single-player test.
- server-browser listing: `getinfo`/`getstatus` still advertise protocol 1, so a
  1.7 browser may hide this server. Use `/connect`.
- CoD4x clients. CoD4x 21.1 uses its own entity decoder and still desynchronises;
  it remains a separate target.
- 1.8 clients, which advertise protocol 7 and are not accepted.
- Windows builds of this server.

Playable compatibility with retail 1.7 clients therefore rests on one local
single-player session, not on a general claim.

The opt-in installed-assets regression exercises real UDP connection admission
without claiming gameplay compatibility:

```sh
python3 scripts/tests/test_linux_handshake.py \
  "$HOME/.steam/debian-installation/steamapps/common/Call of Duty 4" \
  --binary bin/KisakCOD-dedi
```

It stages asset symlinks and a separate temporary home, then starts four servers:
default authentication, the loopback test-mode opt-in, that opt-in plus
`sv_allowProtocol6`, and `sv_allowProtocol6` on its own. It checks default
rejection, unsafe startup binds, source/token/protocol negatives, startup-only
protection and safe socket restart. The protocol-6 session connects with protocol
6, receives `connectResponse` and shows the synthetic `local-test-…` identity in
the server log, while protocols 7 and 21 still receive the two-version error; the
`sv_allowProtocol6`-only server still refuses every client because authentication
is unchanged. The test decodes no gamestate and runs no graphical client, so it is
an admission check rather than a playable-client test.

## Supported boundaries and remaining verification

- Networking is native nonblocking IPv4/UDP. IPX and SOCKS proxying are not
  implemented. If SOCKS is requested, the server warns and uses direct UDP.
- File reads use POSIX positional I/O. The database's completion callback is
  deferred until its alertable wait; disk transfer itself is synchronous, not
  overlapped with decompression. This preserves the current single-read pipeline
  but may change loading performance.
- Rendering, audio playback/capture, cinematics, local-player input and the
  interactive/remote script debugger are excluded. Sound-channel metadata,
  shellshock configuration, script compilation, and the script VM remain.
- Native Steam GameServer initialization and callback-based ticket verification
  remain to be implemented and tested with the official Linux runtime. There is
  no automatic authentication fallback; only the explicit local test mode above.
- Windows source paths are retained, but native MSVC/MinGW builds must still be
  checked in their own environments after shared-header changes.

Before calling this port playable in general, verify native client
authentication, entity/snapshot serialization across several real clients and
maps, downloads, and multiplayer gameplay. One retail 1.7 client has completed a
short local test-mode session; multi-player load, map rotation with a connected
client, CoD4x and 1.8 clients, and Windows builds of this server remain open.
Initial-zone loading/unloading, map start/restart/rotation, loopback queries,
console/signal shutdown, and bounded idle resource sampling have passed the
checks above; longer runs and client load are separate validation requirements.
