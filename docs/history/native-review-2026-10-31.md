# Native follow-up validation — 2026-10-31

> **Historical checkpoint — not current state.** These are this review branch's
> native results on its own date; its counts and limits are not today's tree.
> Current: [2026-11-01 checkpoint](../progress-2026-11-01.md). Current guides:
> [Linux dedicated server](../linux-dedicated.md),
> [Server-side test clients](../server-test-clients.md).

Reviewed branch: `dot/overnight-linux-review-2026-10-31`, baseline delivery
`fd4d8da3`. No merge into master is part of this work.

## Input ordering repair

The SDL adapter previously applied mouse movement immediately while queuing keys
and clicks. A later movement in one SDL pump could therefore change focus before
an earlier click was dispatched. Mouse movement and pointer enter/leave now use
the same ordered engine queue as keys and text. Both Linux host drains dispatch
these records. Focus loss discards pending motion and resets held input.

Consecutive motion records coalesce, without crossing a key or pointer boundary.
When the ring is full, admission of any event first reclaims queued motion rather
than evicting a key release. If no motion can be reclaimed, incoming motion yields;
other event types retain the prior overflow behavior. This does not promise
lossless input under arbitrary transition-only overload. Windows event enums and
structure layout are unchanged by the Linux-only additions.

Regressions exercise the actual SDL pump and a main-loop-equivalent drain:
click before later motion, keyboard before motion, pointer boundaries, focus-loss
cleanup, consecutive motion pressure, and interleaved motion/wheel pressure that
previously lost a key release. The production client-input fixture also exercises
its actual dispatch and mouse consumer.

The full engine event-loop path is not enabled by this change: `Sys_GetEvent`
does not dispatch these new records. The optional hosts use their explicit
`Sys_PollQueuedEvent` drains. Full client-frame integration remains future work.

## Results reproduced directly on this machine

All execution below used native ELF32 binaries, not QEMU, with Clang 19 and the
installed i386 SDL2 development package. Build directories are isolated from the
existing dedicated binary in `bin/`.

- Debug client-core checker: **518 checker checks, zero failures**, including
  linked codec/connection fixtures, **495 parser + 23 input checks**, and eight
  headless UDP/event tests. Counts overlap; do not sum them as unique coverage.
- Debug full window checker: **182 checker checks, zero failures**. Included
  **69 window checks and 64 production-input checks**, plus command/runtime,
  string ownership/failure, rendering, resource and audio fixtures. The existing
  dedicated binary path was unchanged.
- Server function tests: 32-bit fatal ASan/UBSan bot invariants and mutation
  controls passed; weapon-definition ownership tests passed. Host sanitizer
  database publication tests passed. Bot-driver tests: **17 passed**.
- Fresh dedicated build and licensed-assets live fixture: **three bots, three
  full-capacity cohort refills, two map cycles**, five-second holds per stage;
  **12 admissions**, retained final-cohort identities across transitions,
  authentication opt-ins both zero, **clean exit 0**. Completed in **133 seconds**.
  No assertion, fatal error or script error was reported. This is not human-client
  synchronization, bot AI or authentication verification.
- X11 checks use a private Xvfb server and XTest input, not physical keyboard or
  mouse hardware. Engine input, console editing/commands, loopback discovery,
  relative capture, resize/focus, pixel presentation and shutdown passed.
- A freshly built one-shot exporter read the authorized installation into private
  temporary outputs. Bundle/audio/composition/image preparation succeeded. The
  genuine-menu X11 interaction/lifecycle and independent-label check passed.
  Dummy audio is lifecycle evidence, not physical audibility.

## Failed attempts and limits

The first core-check attempt hit its outer 120-second timeout; a longer retry
passed. Window setup initially lacked an i386 SDL development package. The user
installed `libsdl2-dev:i386` and `xdotool`; no dependency change is committed.
An early input-patch compile failed because its header lacked `<cstdint>`; fixed.
Two aggregate build attempts crashed Clang 19 itself (exit 139, lexer stack) in
different translation units. Compiler reproducers were retained under `/tmp`;
subsequent build/check execution passed. These failures are not counted as passes.

This follow-up does not repeat the complete suite in RelWithDebInfo, verify
Windows builds, test physical audio, or establish a playable Linux client.
SDL2 remains the current platform backend, not a decision about the eventual
GPU/world renderer. Installed game assets were read only; configuration, logs,
exports and generated assets stayed in isolated temporary directories and are
not committed.
