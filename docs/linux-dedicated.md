# Native Linux dedicated server

The native Linux target is a **32-bit x86 multiplayer dedicated server**. It
produces an ELF executable, not a Windows executable, and does not use Wine.
The Windows MP/SP/editor build paths remain separate.

This is an initial port, not a gameplay-ready release. There is no native Linux
graphical client. In particular, **Steam authentication is not implemented on
Linux yet: every client ticket is rejected**. Do not deploy this build as a
public game server or treat the bootstrap check as a multiplayer test.

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
  no authentication-bypass fallback.
- Windows source paths are retained, but native MSVC/MinGW builds must still be
  checked in their own environments after shared-header changes.

Before calling this port playable, verify native client authentication and
connection, entity/snapshot serialization with real clients, downloads, and
multiplayer gameplay. Initial-zone loading/unloading, map start/restart/rotation,
loopback queries, console/signal shutdown, and bounded idle resource sampling
have passed the checks above; longer runs, client load, and Windows builds remain
separate validation requirements.
