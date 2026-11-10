# Joining with a key code (no Steam)

A KisakCOD client can join a server with a CoD4 key code ("CD key") instead of
a Steam ticket. The server must opt in with `sv_allowUnverifiedGuids 1`
([server side](linux-dedicated.md#unverified-retail-guids-and-sv_allowunverifiedguids));
stock retail 1.7 clients without Steam already join such servers the same way.

## What it does and does not prove

The client sends `getchallenge 0 "<32 hex digits>"`, the retail two-field
handshake. The 32 digits are a GUID derived from the key code with a one-way
hash: the first half of SHA-256 over `KisakCOD key code GUID v1`, a zero byte,
and the 16-character key. The key itself is never sent or printed by the
game, and it is stored only where you put it: in `codkey.txt`, by hand or
through the game's key code menu.

Nothing checks the key against Activision, so a server can't tell a real key
from a made-up one, and anyone can send another player's GUID. The GUID gives
a player a stable name for a server's ban list and stats, nothing more; see
the server-side warnings. The GUID is not the one retail CoD4 or PunkBuster
derives from the same key, so it does not match retail ban lists.

Servers that require Steam refuse the request with "This server requires Steam
authentication; unverified retail GUIDs are not accepted."

## Setting it up

Either type the key code into the game's own key code menu, or put it in
`codkey.txt` in the KisakCOD home folder (`fs_homepath`) yourself:

```sh
printf '%s\n' 'XXXX-XXXX-XXXX-XXXX-XXXX' > ~/path/to/home/codkey.txt
chmod 600 ~/path/to/home/codkey.txt
```

- 20 letters and digits, dashes and spaces optional, any letter case. The
  last four are a checksum of the first sixteen (CRC-16, as retail checks it),
  so a typo is caught before connecting.
- On Linux and macOS the file must be yours and readable only by you
  (`chmod 600`); otherwise the client refuses it.
- On Windows, if there is no `codkey.txt`, the client reads the retail
  install's registry value (`HKLM\SOFTWARE\Activision\Call of Duty 4`,
  `codkey`).
- Steam owners find their key in the Steam library: right-click Call of Duty 4,
  Manage, CD keys.

### The key code menu

The menu's five fields take the key as printed. When the menu checks it:

- A correctly typed key is saved as `codkey.txt` in the home folder, replacing
  any earlier one, readable only by you (mode 600 on Linux and macOS; the home
  folder is created if it is missing). The console prints where it went and the
  GUID servers will see, never the key. The fields are then emptied, so the key
  does not stay in the game's variables.
- A mistyped or incomplete key is refused and nothing is written; the fields
  keep what you typed so you can fix it.
- With the fields empty, it reports whether a usable key is already stored.

Unlike retail, the menu opens empty: a stored key is never read back into the
fields. "Valid" here means the key's own check characters match, and nothing
more; no key is checked against Activision.

## When it is used

On `connect` to a remote server:

1. A build with Steam (32-bit Windows, or Linux with `KISAK_LINUX_STEAM`) uses
   a Steam ticket when Steam is running.
2. Otherwise, or with `cl_keyCodeOnly 1`, it uses the key code. The default
   native Linux build and 64-bit Windows build have no Steam, so they always
   use the key code.
3. With neither, the connect is refused and the console says where the key
   code goes.

On success the console prints
`[Kisak] Joining with the key code from <file>, as GUID <guid>.` Joining your
own listen server (`localhost`) never asks for either.

`cl_keyCodeOnly 1` (saved in the config) is for a Steam user joining a server
that accepts only GUIDs, such as a default native Linux dedicated server with
`sv_allowUnverifiedGuids 1`.

## Verified

2026-11-08, David's laptop (x86_64 KisakCOD-mp and KisakCOD-dedi, default
builds without Steam, 127.0.0.1, a made-up key with a valid checksum): with
`sv_allowUnverifiedGuids 1` the client joined with its GUID, spawned,
disconnected and joined again with the same GUID. Without the opt-in the
server refused it ("unverified retail GUIDs are not accepted"). With no key
file, a group-readable file or a mistyped key, the client refused to connect
and said why, and nothing reached the server. The key text appeared in no log.
Not yet run: a Windows client (registry or file), a real key, and the key code
menu in the game. Its saving and refusing are covered by the checks below, with
made-up keys, but nobody has typed a key into the menu on screen yet, and where
the stock menus offer it has not been looked up.

## Checks

`scripts/tests/test_linux_client_core.py` runs the client's connect command and
resend loop against key files: a valid made-up key starts the connection and
sends the GUID request; a mistyped, malformed or group-readable file is
refused; the key never appears in the request or on the console. It also
checks SHA-256 and the CRC-16 check value. For the key code menu it checks that
a typed key is saved in five dashed groups with mode 600 whatever the umask,
that a mistyped or short entry leaves the stored key alone, that a file others
can read, a link in the file's place and a leftover from an interrupted save
are replaced and never written through, that a missing home folder is created
private, that an unwritable place is reported, and that neither saving nor
refusing prints the key.
