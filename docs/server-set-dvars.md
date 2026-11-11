# Settings a game server may not set

A game server sets dvars on the clients that join it, by name. This is retail
behaviour and mods depend on it, so it is kept, with one exception: a short
list of settings that belong to the player's machine or to this port. A server
that names one of them is ignored for that name, and the console says so once.
This holds on every platform's multiplayer client.

## How a server sets a client's dvars

| Route | Engine function | What a server can name |
| --- | --- | --- |
| systeminfo | `CL_SystemInfoChanged` | Stock servers send the 20 settings the engine flags `DVAR_SYSTEMINFO` (`sv_pure`, `sv_cheats`, `fs_game`, the file lists and so on). Scripts cannot add to them, but the client sets whatever pair arrives |
| codinfo | `CG_ParseCodInfo` | The settings flagged `DVAR_CODINFO` (about 94 in the engine), plus any name a script passes to `makeDvarServerInfo` |
| `setClientDvar` | `CG_SetClientDvarFromServer` | Any name, on one player. Mods use it for their menus and their own settings |

All three are internal sets: the read-only, start-up-only and cheat flags do
not apply, and a name the client has not registered yet is created and keeps
its value when the client registers it later. Retail trusted the server with
all of it. Two things followed from that: a server could set `cl_updatefiles`,
which the Windows client opened with ShellExecute, and it could set
`activeAction`, whose text the client runs as console commands when the player
enters the game.

## What is refused

All three routes now go through `CL_SetDvarFromServer`
(`src/client_mp/cl_parse_mp.cpp`), which asks the rule in
`src/qcommon/server_dvar_guard.h`. The rule is two lists of names, matched the
way the engine matches dvar names (without regard to letter case), so it holds
for a setting the client has not registered yet.

**Settings this port added** (`kServerDvarsOfThisPort`): every one of them, 50
today: the renderer choice and the Vulkan and GPU wait settings (`r_backend`,
`r_vk*`, `r_gpuSyncSleep*`), the controller and Vita settings
(`in_gamepad*`, `vita_*`), `cl_keyCodeOnly`, `con_restricted`, `snd_adpcm`,
`snd_alDebug`, `xanim_blendOnce`, and the settings for how the player's own
server admits players (`sv_allowUnverifiedGuids`, `sv_allowProtocol6`,
`sv_advertisedProtocol`, `sv_loopbackTestAuth`). No retail server or mod knows
these names.

**Retail settings that belong to the machine** (`kServerDvarsOfTheMachine`):

| Settings | Why a server may not set them |
| --- | --- |
| `activeAction`, `nextdemo` | Their text is run as console commands (on entering the game, at the end of a demo) |
| `masterServerName`, `masterPort`, `authServerName`, `authPort` | Which hosts the client contacts |
| `cl_updatefiles`, `cl_updateavailable`, `cl_updateversion`, `cl_updateoldversion` | The update the client offers to fetch and open |
| `cl_allowDownload`, `cl_wwwDownload` | The player's consent to downloads. Systeminfo is read before the client decides whether to download what the server names, so a server could switch downloads on for a player who had them off |
| `fs_basepath`, `fs_homepath`, `fs_cdpath`, `fs_basegame`, `fs_copyfiles`, `fs_restrict`, `com_playerProfile` | Where the game reads and writes files |
| `net_ip`, `net_port`, `net_noudp`, `net_noipx`, `net_socksEnabled`, `net_socksServer`, `net_socksPort`, `net_socksUsername`, `net_socksPassword` | The player's network set-up |
| `rcon_password`, `g_password`, `sv_privatePassword`, `sv_wwwBaseURL`, `sv_wwwDownload`, `sv_wwwDlDisconnected` | The player's own server: its remote console, its passwords and where it sends players for downloads |

`fs_game` is not on the list: a server names its mod with it. No listed name
is one the engine replicates to clients, so a stock server never sends a name
that is refused.

What stays as in retail:

- Every other name, on all three routes. A mod's `setClientDvar` and
  `makeDvarServerInfo` calls work as before.
- A listen server is the player's own, and its scripts set anything: the server
  runs in this process and the client is joined to it over the local address.
  A demo is treated as a remote server, whatever runs locally.
- A mod the player has installed or downloaded can still run console commands
  through its own menus. The list protects the routes that need no consent from
  the player, and it keeps `cl_allowDownload` the player's own choice.

## The systeminfo note

A stock server sends through systeminfo only the settings the client itself
flags `DVAR_SYSTEMINFO`. When a systeminfo pair names anything else, the client
still applies it and prints one line per name:

```
Note: the server's systeminfo set <name>, which is not a systeminfo setting on this client. It was applied.
```

Nothing is refused by this rule yet. It is there to learn from play on real
servers whether refusing such pairs would be safe; if no server ever causes the
line, systeminfo can become a real allow-list.

## Adding a dvar

A new dvar this port registers must be added to `kServerDvarsOfThisPort`. The
asset-free check `scripts/tests/test_server_dvar_guard.py` (runner check
`server_dvar_guard`) fails with the name and the file when one is missing: it
compares the names registered in `src` with the retail names in
`scripts/tests/retail_dvar_names.txt`. If servers have a reason to set the new
dvar, exempt it in the check's `PORT_DVARS_A_SERVER_MAY_SET`, with the reason.

New client code should still not read a dvar at the moment it runs a command,
opens a file or an address, or picks a host. The list is a second line of
defence, not a reason to drop the first.
