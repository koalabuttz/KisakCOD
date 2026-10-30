Native Linux dedicated-server checks are documented in
[the Linux build guide](../../docs/linux-dedicated.md#asset-free-checks). They
include an ELF32/bootstrap smoke test, native runtime and UDP tests, fastfile
short-read regressions, and non-interactive debugger checks. They require the
32-bit Linux toolchain and are separate from Windows runtime checks. After
building, run `python3 scripts/tests/run_linux_checks.py --build-dir build-linux
--binary bin/KisakCOD-dedi` for the full asset-free set. The opt-in
`test_linux_server_lifecycle.py` additionally exercises restart/rotation, live
loopback queries and resource samples with installed licensed assets; it is not
run on public CI. Its failure-cleanup/query-parser unit tests are asset-free.
`test_server_handshake_auth.py` exercises production authentication parsing,
handshake functions and connect-version admission (including the opt-in
`sv_allowProtocol6` switch) without assets; the UDP suite also checks the
startup-only loopback bypass against real sockets. The opt-in
`test_linux_handshake.py` uses installed assets to verify real UDP admission,
default rejection, unsafe bind failures and optional protocol-6 admission for a
local-test client. It decodes no gamestate and does not establish general
stock/CoD4x client compatibility.

These shared asset-free regression checks require Python 3 and a C++20 compiler:

```sh
python3 scripts/tests/test_renderer_command_ownership.py
python3 scripts/tests/test_script_vm_field_stack.py
python3 scripts/tests/test_script_lexer_eof.py
python3 scripts/tests/test_shader_constant_order.py
python3 scripts/tests/test_shellshock_channel_metadata.py
python3 scripts/tests/test_script_loading_watchdog.py
python3 scripts/tests/test_voice_lifecycle.py
python3 scripts/tests/test_script_console_watchdog.py
python3 scripts/tests/test_exec_command_ownership.py
```

All scripts accept `--cxx` (or `CXX`) to select a compiler. They compile the
functions extracted from the current production source, with checked test
doubles for unrelated engine services. Assertions remain enabled.

The renderer check covers command-buffer ownership before initialization,
partial allocation, unvisited and aliased frame bindings, normal/swapped frame
cycles, and command-buffer cleanup/reallocation. Lighting, debug geometry and
scene cleanup are stubbed; complete renderer shutdown still needs a game check.

The lexer EOF check runs the production scanner/parser with zero and one local
clients in SP and MP. Empty/comment-only files, final tokens without a newline,
repeated parses, and malformed input must terminate without restarting an
exhausted source. It does not require a running client or game assets.

The VM check forces the global layout that exposed the false overflow, then
checks scalar and undefined fields, array getters that need two working values,
repeated getters, output-parameter cleanup and genuine stack overflow. It tests
the field bridge and stack bookkeeping, rather than executing complete scripts.

The shader check verifies destination ordering and destination/value pairing
through all 16 constant slots, with unused storage poisoned to expose accidental
reads. It exercises the ordering used by material comparisons; loading and
rendering real materials still need a game check. The extracted collection and
comparison helpers also check literal and named constants, equivalent/self
comparisons, reversed ordering signs and repeated comparisons.

The separate [D3DX runtime check](d3dx_shader_runtime.md) compiles and reflects
real shaders on Windows or Wine, including macros, includes, input/output
semantics, COM ownership and invalid-input diagnostics. It needs the x86 D3DX9_43
runtime but no GPU or game assets. Windows CI runs it in both configurations.

The shellshock check loads sound-channel metadata without an audio device,
verifies repeated/client reuse and fallback settings, and preserves warnings for
unknown channel names and errors for missing channel definitions.

The watchdog check excludes synchronous item-registration time during loading
without resetting script time already spent. It checks SP and MP precache calls,
the five-second boundary, real pure/interleaved loops, clock wrap and unchanged
runtime/precache restrictions.

The console watchdog check applies the same loading-time accounting to script
print output. It covers SP/MP print and println, profiling output, suppressed
messages, nested accounting, clock wrap and genuine repeated-print loops. It
preserves the script budget already consumed and normal runtime timing.

The voice check exercises disabled/dedicated startup, device and codec failure
rollback, repeated shutdown, off/on transitions and capture failures. It also
checks COM buffer ownership and bounded sample storage through 1,000 capture
stop/start cycles. Audio devices and codec allocation use checked test doubles;
physical microphone/playback behavior still needs a device test.

The exec check runs production disk/fastfile dispatch and command parsing across
map-memory resets and asset unloading. It covers trailing commands, nested
configs, missing/empty input, allocation failure, startup order and repeated
restarts while preserving synchronous execution and existing wait semantics.
Real nonlocal aborts also check reverse-order ownership cleanup, repeated and
reentrant recovery, and bounded command nesting.

GitHub Actions runs these checks alongside the Windows builds and native
bootstrap check. Loading a map and quitting with licensed assets remain separate
application checks.
