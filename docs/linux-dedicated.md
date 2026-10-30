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

Runtime and UDP checks need the same multilib toolchain, but no game assets:

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

A separate `mp_crash` test loads the map fastfile and collision data and reaches
`Game Initialization`, but stalls there and was terminated after 90 seconds.
**Map startup and gameplay are not yet working/verified.** Diagnostic logs for
that local run are `build-linux/steam-map.log`. Further investigation needs a
thread backtrace (install `gdb` if it is not available).

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

Before calling this port playable, verify with licensed assets: complete zone
loading/unloading; map start/restart/rotation; sustained server simulation;
client authentication and connection; entity/snapshot serialization; downloads;
console `status`/`quit` with an active map; signal shutdown; and resource cleanup
over repeated maps. Initial-zone loading/unloading and a no-map `quit` have passed
the optional installed-assets check above; that does not establish the remaining
gameplay checks.
