# Supported offline renderer frame ownership

Both frontend paths—the genuine menu owner and ordinary image/text presentation—now use `CpuRenderer::BeginScopedFrame(window)` and `Frame::Finish()`. The scope operates the existing renderer state machine and actual shared command producers; it is not a second renderer or a full game-frame service.

A scope begins recording. `Finish` appends the real end command, validates/replays the supported list, and presents through the actual SDL window surface. Successful presentation alone advances the counter. Abandoning the scope, a recording exception, or failed finish cancels its pending work and returns the renderer to ready while retaining owned textures/materials/fonts. Cancellation does not promise to restore CPU pixels after replay or SDL failure. The original exception is rethrown.

Frame scopes are noncopyable and nonmovable. The renderer and window must outlive them. Recording, finishing, and destruction belong to the renderer owner thread. Wrong-thread finish fails without cancelling owner-thread work. An off-thread destructor reports the contract violation and does not mutate owner-thread recording; the owner must explicitly cancel it. No cross-thread renderer safety is claimed.

Generation identities prevent stale scopes from finishing or cancelling replacement work. Successful resize—including same-size resize—explicitly cancels pending work and invalidates its scope. Failed resize retains the existing frame. Cancel, presentation and shutdown also invalidate scopes; generation identity never resets across renderer reinitialization. The counter saturates rather than wrapping, and future frame starts fail closed on exhaustion. Explicit cancellation of an already finished or invalidated scope is idempotent.

Two concrete low-level ownership gaps were reproduced before the fix:

- External `SubmitCommands` could replace an in-progress owned recording. It now requires ready state.
- Failed `Submit` left its appended end command in a supposedly recording list. Seven failures consumed one clear-command's worth of capacity. Submission now restores recording metadata on failure, including the critical-space count and last-command pointer. The original preflight error and unchanged pixels are preserved.

These low-level methods remain available with their documented state contracts. Resource release remains ready-only. The scoped path is used by both production offline frontends so they share finish/cancel behavior instead of relying on separate error-handling conventions.

The focused fixture executes actual command producers, CPU replay and SDL dummy-surface presentation. It covers baseline failures, abandoned/throwing recording, nested scopes, wrong-thread finish, stale generations across cancel/resize/restart, ready-state stale resource handles, quit/presentation failure, and window recreation. Dummy SDL validates software lifecycle, not hardware display behavior; genuine X11 and asset-driven integration remain separate checks. Existing allocation-fault tests cover resize and resource failure paths.

Full `R_Init`, renderer device/world startup, `CL_Frame`, authentication and packet processing remain outside this service. Existing partial-frame and unrendered-blur reporting is unchanged.
