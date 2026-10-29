These asset-free regression checks require Python 3 and a C++20 compiler:

```sh
python3 scripts/tests/test_renderer_command_ownership.py
python3 scripts/tests/test_script_vm_field_stack.py
```

Both scripts accept `--cxx` (or `CXX`) to select a compiler. They compile the
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

GitHub Actions runs these checks alongside the Windows builds and native
bootstrap check. Loading a map and quitting with licensed assets remain separate
application checks.
