# Client parser/input checkpoint — 2026-10-31

Local branch: `dot/client-parser-2026-10-31`, independently based on master
`f8bde51a`. Implementation: `b611d4cf`. Server reliability commits remain on
`dot/server-reliability-2026-10-31`; neither branch has been pushed or merged.

## Confirmed bug and fix

`CL_ParseCommandString` read a sequence and string and then immediately updated
reliable-command history. `MSG_ReadString` marks overflow when input ends but
still returns a terminated partial string. Thus a truncated newer command could
advance `serverCommandSequence` and overwrite its ring slot, even though
`CL_ParseServerMessage` subsequently discarded the overflowed message.

The fix checks `msg->overflowed` before looking up/mutating the connection. Valid
command behavior is unchanged, including duplicate/stale rejection and the
existing bounded-string policy. The surrounding message loop still owns discard
handling; no new authentication, protocol or error-policy behavior was added.

The first harness linked unchanged production parser/readers and failed 18 of
475 assertions. The expanded final harness also rejects the old parser: 28
failures, including complete retransmission of a previously malformed sequence.

## Runtime coverage

The optional `KisakCOD-client-state-test` links full production translation units
`cl_parse_mp.cpp`, `cl_input.cpp`, `msg_mp.cpp`, and `q_shared.cpp`. It uses
production headers, readers, character cleanup and bounded copying. Unused
engine subsystems are removed by section GC; production function bodies are not
extracted or rewritten in the regression.

Parser coverage includes advancing/duplicate/stale sequence handling, slots
127/128/129, every truncation of a command, complete same-sequence retransmission,
preexisting overflow, empty and oversized text, NUL termination, preservation of
the following byte, every split-buffer boundary, character cleanup, prior
history, and connection fields after the final ring slot.

Input coverage runs real key-down/up functions, key fractions, attack wrappers,
command-button updates and clear-keys. It covers repeated presses, two sources
for one button, third-source rejection, unrelated releases, sampled/unsampled
held time, missing timestamps, fraction clamping, short pulses, held flags and
clearing. No input production behavior was changed.

The boundary doubles are connection lookup, command arguments, frame clocks,
diagnostic output and fatal assertion handling. No socket, window, physical
input device, prediction loop, snapshot parsing or graphical gameplay was run.

## Results and limitations

| Check | Result |
| --- | --- |
| Debug client checker | 91/91 passed |
| RelWithDebInfo client checker | 91/91 passed |
| Codec runtime on each build | 25 assertions passed |
| State runtime on each build | 495 parser + 23 input assertions passed |
| Runtime evidence validator | 8 unit tests passed |
| Compiler mapping checker | 12 unit checks passed |
| Old production parser, scratch negative control | Failed normally, 28 assertions |
| Input pulse-consumption removal, scratch negative control | Failed normally, 1 assertion |
| Input time-consumption removal, scratch negative control | Failed normally, 5 assertions |

Both full checker runs include real production compile-command validation,
active assertions, incremental rebuilds, absent targets in default-OFF builds,
and an unchanged dedicated executable fingerprint. The state result validator
requires both named nonempty sections, zero failures and consistent totals;
negative unit cases also enforce nonzero-process-exit failure. CI is wired to
run these units and the expanded runtime checker, but hosted CI has not run.

As in the server checkpoint, **32-bit execution was through the existing QEMU
i386 setup**, because this environment cannot directly execute ELF32. These are
not native-kernel execution or sanitizer results. No new workaround around that
restriction was attempted. The production client layouts require 32-bit builds;
32-bit ASan is still unavailable under the established QEMU setup. Native
32-bit sanitizer validation and Windows builds remain **not run**.

Local evidence is retained under `/workspace/kisakcod-client-results/`, including
both full checker logs, checker-unit results and all three negative-control
outputs. Builds and local toolchain adapters remain under
`/workspace/native-tools/client-debug` and `client-release`. None of those
artifacts or toolchain adaptations are committed.

Independent review found no remaining actionable production or test findings.
Review recommendations added same-sequence recovery, nonzero trailing canaries
and checker-negative cases. The reviewer independently passed all eight checker
unit tests. This review did not add native sanitizer or graphical validation.

## Remaining work

The client still has no playable executable, native startup/window/input-device
integration or renderer. Next bounded client work can cover command execution or
prediction-history lookup using production translation units; full snapshot and
gamestate parsing need their own explicit state/ownership tests.

Server follow-ups remain open: native 32-bit full-suite validation, connected-bot
map transitions, death/respawn and drop/refill. The one debugger-attached QEMU
`!g_loadingAssets` assertion is retained as unresolved; this client milestone
neither investigates nor closes it. No real two-player or mixed-client gameplay
claim is made.
