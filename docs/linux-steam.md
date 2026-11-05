# Steam authentication on Linux (opt-in)

`-DKISAK_LINUX_STEAM=ON` gives the native Linux builds (`KisakCOD-mp` and
`KisakCOD-dedi`, i386 and x86_64) the Steam authentication the Windows build
has, so a Linux player can join a Steam-authenticated KisakCOD server and a
Linux server can check Steam tickets. It is off by default; without it Linux
refuses every Steam ticket and the client cannot join remote servers, as
before.

## How it works

CoD4 never had a Linux release, so there is no Linux `libsteam_api.so` for it,
and Steam does not need to ship this build. Steam login does not go through the
game's files at all: the Steamworks calls talk to the Steam client already
running on the player's machine. Every Steam install has the client's own
library for that, `steamclient.so`:

- i386: `~/.steam/sdk32/steamclient.so`
- x86_64: `~/.steam/sdk64/steamclient.so`

At start-up `Steam_Init` (`src/linux/steam.cpp`) sets `SteamAppId` to 7940
(CoD4), loads that library with `dlopen`, connects to the logged-in user and
then makes the same calls the Windows build makes through `steam_api.dll`
(`src/win32/win_steam.cpp`):

- **Client.** `connect <address>` asks Steam for an auth session ticket bound
  to the server's address and sends it, with the SteamID, in the
  `getchallenge` request, in place of a CD key.
- **Server.** `SV_GetChallenge` passes the ticket to `BeginAuthSession`. The
  player is admitted only if Steam accepts the ticket for checking; when Steam's
  verdict (`ValidateAuthTicketResponse_t`) comes back negative, the player is
  dropped. A dropped player's session is ended with `EndAuthSession`.

The connection code is `src/linux/steam/steamclient_session.cpp`. It knows
nothing about the engine. Nothing links against Steam: the build needs only the
SDK headers in `deps/steamsdk`, and no Steam library is committed or bundled.

Set `KISAK_STEAMCLIENT_LIBRARY` to load a different `steamclient.so`.

## What you need

- The Steam client running and logged in, on an account that owns CoD4.
- For a server: the same, on the server machine. The Windows build works this
  way too (it uses the client API, not the GameServer API), so a headless box
  without a Steam client cannot check tickets.

If Steam is not running, not logged in, or the library is missing, start-up
prints why and the backend stays off: the client refuses remote servers and the
server rejects every Steam ticket. There is no fallback that admits players
without a ticket.

Expect Steam to show you as playing Call of Duty 4 while the game runs.

## Build

Add `-DKISAK_LINUX_STEAM=ON` to the usual configure line of
[`KisakCOD-mp`](linux-mp-client.md) or the
[dedicated server](linux-dedicated.md).

## Checks

`scripts/tests/test_linux_steam_session.py` (also run by `run_linux_checks.py`
as `steam_session`) needs no Steam:

- It builds a stand-in `steamclient.so` from `tests/linux_steam/` and drives the
  real session code against it, 64-bit and 32-bit: library lookup, each way
  opening can fail and what gets released, the app id Steam sees, ticket
  requests bound to the server's address, `BeginAuthSession`, and callback
  reading (verdicts passed on, every callback released).
- It compiles the units the flag changes with `KISAK_LINUX_STEAM`, in both the
  dedicated server's and the client's definitions.

## Verified

- 2026-11-05, David's laptop (Debian 13, Steam running, CoD4 owned): the
  standalone probe `scripts/tools/linux_steam_probe/steam_probe.cpp` connected
  through `~/.steam/sdk32/steamclient.so` and `sdk64` as app 7940, was logged on
  and subscribed, got a 234-byte ticket, and Steam validated it
  (`BeginAuthSession` OK, then `ValidateAuthTicketResponse` OK).
- 2026-11-05, laptop game run 1 (PR 85 first commit, i386 dedi and client, both
  flag on, same Steam account): the client joined the dedicated server on
  127.0.0.1:28960; the server logged "A player passed the Steam auth check";
  the client reached the game and spawned. With the server's Steam backend off
  (`KISAK_STEAMCLIENT_LIBRARY=/nonexistent/...`), the same client was refused
  with "Your Steam Client Ticket was Invalid". That run found two bugs, fixed
  since: Steam started after the command line ran, so `+connect` on the
  command line could not authenticate (Steam_Init now runs before Com_Init, as
  in WinMain); and every Steam-on process crashed on quit, releasing its Steam
  pipe from an exit-time destructor after steamclient.so's own teardown
  (Sys_Quit now shuts Steam down first, and the destructor never calls Steam).

Steam delivers auth verdicts to every process logged in to the same account,
so each process acts only on verdicts for SteamIDs it started a session for. A
player who reconnects before their old session ended (a listen server does not
end it on drop) gets the old session ended and the new ticket checked.
