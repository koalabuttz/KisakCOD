# Native client frame startup dependencies

The standalone host can now use the complete, unchanged production
`CL_InitInput`/`CL_ShutdownInput` lifecycle. This replaces manually registering
four IN handlers. It is a real startup prerequisite called by
`CL_InitOnceForAllClients`; it does not mean `CL_Init` or `CL_Frame` has run.

## Confirmed input startup boundary

`CL_InitInput` registers the full production movement, action, stance, ADS and talk
command set, then registers `cl_analog_attack_threshold`, `cl_stanceHoldTime` and
`cl_nodelta`. These handlers use actual `playersKb` and owned client state. Their
connected-state guards remain unchanged. `+talk` changes the talk key state; it
neither starts voice capture nor initializes audio. The standalone host supplies
the normal `cl_freelook=true` setting and a real monotonic `com_frameTime` clock;
`cl_nodelta` is populated by production initialization. No replacement input
implementation or success stub is needed.

The focused `input_startup_test.cpp` verifies the complete registered command set,
production dvar defaults, real Cmd/Cbuf action/release/stance behavior, pending
command cancellation on focus loss, input-only shutdown while the command runtime
remains alive, and repeated initialization/ownership. All public execution stays
`CA_DISCONNECTED` with `isRunning`, renderer and game UI uninitialized.

## Why full startup/frame execution is still unavailable

| Production entry point | Concrete dependencies | Current boundary |
| --- | --- | --- |
| `CL_InitOnceForAllClients` | `Con_Init`, full input initialization, broad client dvars/commands and developer facilities | Full input lifecycle is now real; the rest is not claimed |
| `CL_Init` | `CL_ClearState`, muted-list state, command execution and startup globals; sets `isRunning=1` | Not called simply to flip its running flag |
| `CL_ClearState` | Client state teardown and DObj/skeleton-related state | Requires actual client/game memory ownership before use |
| `CL_Frame` unconditional prefix | `Hunk_CheckTempMemoryClear`, `Hunk_CheckTempMemoryHighClear` | Real hunk arenas are not initialized by the standalone runtime |
| `CL_Frame` running prefix | DevGui frame, voice capture/playback, color/userinfo state | No DevGui/audio success substitutes are provided |
| `CL_Frame` connection maintenance | Actual resend/timeout state, owned connection dvars, authentication provider | Public host remains disconnected; provider-unavailable policy remains closed |
| `CL_Frame` remaining phases | Download, in-game state, `CL_SetCGameTime`, command creation/transmission and key authorization | Cgame/asset/time and authentication startup remain unfinished |
| `CL_RunOncePerClientFrame` / renderer startup | UI, input platform frame and GPU registration | SDL surface presentation does not initialize the D3D9 game renderer |

`CL_Frame` keeps its production order. Calling it with `isRunning=false` would
only exercise the hunk checks, not a functional client frame; setting the flag
without the services above would be unsafe. The standalone input tick and existing discovery polling are explicitly named
as such and are not reported as `CL_Frame` execution. Central packet-dispatch
integration remains paused; this prerequisite adds no receive path.

The next useful startup step is genuine ownership of the remaining memory and
client services, or a narrowly shared transport-maintenance phase with its real
dvars and authentication lifetime. Neither requires weakening packet admission or
adding fake renderer/voice/cgame handlers.
