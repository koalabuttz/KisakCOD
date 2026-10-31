# Native client interned-string ownership

The optional Linux client Runtime initializes actual engine `SL_Init` (which
initializes `MT_Init`) before registering dvars. Existing owners release input
bindings and UI local variables before Runtime teardown; Runtime shuts down
commands and dvars before invoking real `SL_Shutdown` reference/leak checks.
This is one startup prerequisite, not full `Com_Init`, `CL_Init`, scripting VM
initialization or a playable game frame.

`CopyString`, `FreeString` and `ReplaceString` moved unchanged from
`com_memory.cpp` to `com_strings.cpp`, shared by the default game and native
Runtime. Identical strings share storage while each owning acquisition retains
its reference. Dvar current/latched/reset fields retain production weak-alias
rules; names have independent ownership even when their contents match the
value. `ReplaceString` retains its null-old guard and acquires the replacement
before releasing the old reference.

Raw string pointers and IDs are valid only while their references remain owned.
They are not generation-checked handles. Callers must clear borrowed references
on reload/shutdown and cannot use them after the last release or pool shutdown.
Owned UI registration releases local strings and clears borrowed registry/stack
entries before replacement. Teardown does not silently discard leaks: the real
engine assertions remain active.

## Allocation failure

The standalone host reports `Com_Error` by throwing. Real finite-pool exhaustion
exposed two defects in that environment:

* `SL_GetStringOfSize` retained its outer string critical section after MT
  allocation failed. Explicit exception cleanup now releases it. Normal return
  paths remain unchanged. The catch handles C++ exceptions only; it does
  not provide longjmp cleanup or introduce an RAII guard.
* `Dvar_RegisterNew` published its pool slot/count before allocating owned name
  and value strings. Failure retained the write lock and could leak the name.
  It now prepares a POD candidate, publishes after successful allocation, and
  releases each acquired reference and the lock on an exception. Successful
  numeric/vector/string values, flags, domain and hash ordering are preserved.

This does not promise recovery from every legacy dvar operation or longjmp
path. Failed string assignments preserve prior owned values. Existing setter
metadata, such as the global modified-flags accumulator, may already have been
touched; the tests do not claim a transaction over unrelated metadata.

## Verification

The optional window build includes `KisakCOD-string-owner-test`,
`KisakCOD-dvar-string-failure-test` (`--name` and `--value`) and
`KisakCOD-string-consumers-test`. These use actual SL/MT exhaustion, a bounded
cross-thread lock probe, actual UI setters/registration and repeated Runtime
teardown. Baselines exit without unsafe cleanup of deliberately stranded locks.
No 32-bit sanitizer is used. The window checker runs all four scenarios;
default-off checks require these optional targets to be absent.

Baseline and final execution evidence accompanies the source-only delivery
package. This change neither modifies
nor retries the denied packet/receive work, and adds no networking,
authentication or full-frame capability.
