# Settings a game server may not set

A game server sets dvars on the clients that join it, by name. This is retail
behaviour and mods depend on it, so it is kept: every retail setting behaves as
it did in retail. The one exception is the settings this port added, which no
retail server and no retail mod knows. A server that names one of them is
ignored for that name, and the console says so once. This holds on every
platform's multiplayer client.

## How a server sets a client's dvars

| Route | Engine function | What a server can name |
| --- | --- | --- |
| systeminfo | `CL_SystemInfoChanged` | Stock servers send the 20 settings the engine flags `DVAR_SYSTEMINFO` (`sv_pure`, `sv_cheats`, `fs_game`, the file lists and so on). Scripts cannot add to them, but the client sets whatever pair arrives |
| codinfo | `CG_ParseCodInfo` | The settings flagged `DVAR_CODINFO` (about 94 in the engine), plus any name a script passes to `makeDvarServerInfo` |
| `setClientDvar` | `CG_SetClientDvarFromServer` | Any name, on one player. Mods use it for their menus and their own settings |

All three are internal sets: the read-only, start-up-only and cheat flags do
not apply, and a name the client has not registered yet is created and keeps
its value when the client registers it later. Retail trusted the server with
all of it.

## What is refused

All three routes now go through `CL_SetDvarFromServer`
(`src/client_mp/cl_parse_mp.cpp`), which asks the rule in
`src/qcommon/server_dvar_guard.h`. The rule is one list of names,
`kServerDvarsOfThisPort`, matched the way the engine matches dvar names
(without regard to letter case), so it holds for a setting the client has not
registered yet.

The list is every setting this port added, 50 today: the renderer choice and
the Vulkan and GPU wait settings (`r_backend`, `r_vk*`, `r_gpuSyncSleep*`), the
controller and Vita settings (`in_gamepad*`, `vita_*`), `cl_keyCodeOnly`,
`con_restricted`, `snd_adpcm`, `snd_alDebug`, `xanim_blendOnce`, and the
settings for how the player's own server admits players
(`sv_allowUnverifiedGuids`, `sv_allowProtocol6`, `sv_advertisedProtocol`,
`sv_loopbackTestAuth`). No retail server or mod knows these names, and none of
them is one the engine replicates to clients.

## What stays as in retail

Everything else:

- Every retail setting, on all three routes. A mod's `setClientDvar` and
  `makeDvarServerInfo` calls work as before, and so does a server that sends
  more through systeminfo than a stock one. The asset-free check feeds every
  retail dvar name (1531 of them) to the rule and fails if one is refused.
- A listen server is the player's own, and its scripts set this port's settings
  too: the server runs in this process and the client is joined to it over the
  local address. A demo is treated as a remote server, whatever runs locally.

That includes retail settings that matter to the player's machine. They were
looked at when the list was drawn up, and the decision was to leave them as the
game shipped rather than risk a mod that sets them:

| Retail setting | What a server can do with it, as in retail |
| --- | --- |
| `activeAction` | Its text is run as console commands when the player enters the game |
| `nextdemo` | Its text is run as a console command at the end of a demo |
| `cl_allowDownload`, `cl_wwwDownload` | Systeminfo is read before the client decides whether to download what the server names, so a server can switch downloads on for a player who had them off |
| `fs_basepath`, `fs_homepath`, `net_*`, `rcon_password`, `sv_www*` and the like | A server can change them like any other setting |

Two guards from earlier work are separate from this list and stay:
`masterServerName` and `masterPort` cannot be changed while the client is
connected to a remote server ([finding servers](server-browser.md)), and an
address the client hands to the desktop, such as the update file a server can
name through `cl_updatefiles`, is opened only when it is a plain `http://` or
`https://` address (`src/qcommon/url_check.h`).

A mod the player has installed or downloaded can also run console commands
through its own menus, as in retail.

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

A retail name does not go on the list, and the check fails when one is added.
Retail settings stay as in retail; changing that for a name is a decision for
the project owner, not for a patch.
