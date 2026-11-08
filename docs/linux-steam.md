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
- For a server: either the same, on the server machine (the Windows build works
  this way too: it uses the client API, not the GameServer API), or, for the
  dedicated server, an anonymous Steam game server login (below).

Steam keeps an account signed in on one computer at a time. With one account, a
player on one machine and a server checking tickets as that account on another
cannot both be online; use the anonymous game server login for the server.

## Anonymous game server login (dedicated server, opt-in)

Set `KISAK_STEAM_SERVER_LOGIN=anonymous` in the dedicated server's environment
and it logs on to Steam as an anonymous game server for app 7940, the way most
Linux game servers check tickets, instead of as the Steam client's user:

```sh
KISAK_STEAM_SERVER_LOGIN=anonymous ./KisakCOD-dedi +set dedicated 2 +map mp_crash
```

- It still loads `steamclient.so` (`~/.steam/sdk32` or `sdk64`, or
  `KISAK_STEAMCLIENT_LIBRARY`), so Steam must be installed, but the Steam client
  need not run, and nobody's account is used.
- The logon finishes a moment after start-up: the server prints
  `[Kisak] Steam: logged on as an anonymous game server for app 7940`. Until
  then, and while Steam is unreachable, it rejects every ticket.
- Tickets are checked with the game server's `BeginAuthSession`; the verdict
  handling is the same as in the client mode.
- The server is not listed in Steam's server browser (the query port is
  shared with the game socket, and nothing answers Steam's queries).
- It is an environment variable because Steam starts before the command line
  runs. Any other value turns the backend off. A client or listen server
  ignores it.

`scripts/tools/linux_steam_probe/steam_probe.cpp gameserver <steamclient.so>`
checks the logon on its own.

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
- 2026-11-05, laptop game run 2 (the fixes): a command-line `+connect` to the
  Steam-on dedicated server authenticated on the first try and reached the
  game; `disconnect` and a console `connect` passed again; the client logged no
  verdicts of its own; with the server's Steam off the client was refused; all
  four processes quit with exit 0.

- 2026-11-08, laptop game run 3 (master d9b78de, x86_64 dedi and client, both
  flag on, same Steam account): both logged on through
  `~/.steam/sdk64/steamclient.so`; a command-line `+connect` passed the server's
  Steam auth check and spawned, and `disconnect` plus a console `connect` passed
  again; with the server's Steam off the client was refused ("Your Steam Client
  Ticket was Invalid") and never entered the game. Mixed builds also passed both
  ways (x86_64 client to an i386 server, i386 client to an x86_64 server; the
  i386 builds were run 2's). Every process quit with exit 0.

- 2026-11-08, laptop probe (`steam_probe gameserver`, x86_64 and i386, twice
  each): Steam accepted an anonymous game server logon for app 7940
  (`SteamServersConnected`, logged on, anonymous game server ID) and rejected a
  junk ticket. The desktop Steam client was not used.

- 2026-11-08, laptop run 17 with the Windows PC (PR 133 at 4ead239, x86_64
  dedi on the laptop's LAN address with `KISAK_STEAM_SERVER_LOGIN=anonymous`;
  a Windows Win32 KisakCOD client on the PC, signed in to the same Steam
  account the laptop's desktop Steam had been using): the server logged on as
  an anonymous game server, the PC joined, and the server logged "A player
  passed the Steam auth check"; the PC reached the game and disconnected
  normally. With the server's Steam backend off the PC was refused ("Your Steam
  Client Ticket was Invalid"). That run found an exit-time crash (exit 139):
  steamclient's own threads for the game server pipe were still running when
  `exit()` tore the library down. Close now calls `BShutdownIfAllPipesClosed`
  after releasing that pipe, as `SteamGameServer_Shutdown` does.
- 2026-11-08, laptop run 19 (PR 133 at 046a161): three anonymous-login quits
  after the logon line, one quit before the logon finished, and one client-mode
  quit all exited 0.

Not verified: a Windows server, two different Steam accounts, and a Windows
client against a Linux server in client mode (one account can't be signed in on
two computers at once, which is why the anonymous login exists).

Steam delivers auth verdicts to every process logged in to the same account,
so each process acts only on verdicts for SteamIDs it started a session for. A
player who reconnects before their old session ended (a listen server does not
end it on drop) gets the old session ended and the new ticket checked.
