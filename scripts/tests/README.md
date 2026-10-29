These asset-free regression checks require Python 3 and a C++20 compiler:

```sh
python3 scripts/tests/test_renderer_command_ownership.py
python3 scripts/tests/test_script_vm_field_stack.py
python3 scripts/tests/test_shader_constant_order.py
python3 scripts/tests/test_shellshock_channel_metadata.py
```

All scripts accept `--cxx` (or `CXX`) to select a compiler. They compile the
functions extracted from the current production source, with checked test
doubles for unrelated engine services. Assertions remain enabled.

The renderer check covers command-buffer ownership before initialization,
partial allocation, unvisited and aliased frame bindings, normal/swapped frame
cycles, and command-buffer cleanup/reallocation. Lighting, debug geometry and
scene cleanup are stubbed; complete renderer shutdown still needs a game check.

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

GitHub Actions runs these checks alongside the Windows builds and native
bootstrap check. Loading a map and quitting with licensed assets remain separate
application checks.
