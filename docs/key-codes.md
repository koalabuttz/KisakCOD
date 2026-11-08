# Joining with a key code (no Steam)

A KisakCOD client can join a server with a CoD4 key code ("CD key") instead of
a Steam ticket. The server must opt in with `sv_allowUnverifiedGuids 1`
([server side](linux-dedicated.md#unverified-retail-guids-and-sv_allowunverifiedguids));
stock retail 1.7 clients without Steam already join such servers the same way.

## What it does and does not prove

The client sends `getchallenge 0 "<32 hex digits>"`, the retail two-field
handshake. The 32 digits are a GUID derived from the key code with a one-way
hash: the first half of SHA-256 over `KisakCOD key code GUID v1`, a zero byte,
and the 16-character key. The key itself is never sent, printed or stored by
the game.

Nothing checks the key against Activision, so a server can't tell a real key
from a made-up one, and anyone can send another player's GUID. The GUID gives
a player a stable name for a server's ban list and stats, nothing more; see
the server-side warnings. The GUID is not the one retail CoD4 or PunkBuster
derives from the same key, so it does not match retail ban lists.

Servers that require Steam refuse the request with "This server requires Steam
authentication; unverified retail GUIDs are not accepted."

## Setting it up

Put the key code in `codkey.txt` in the KisakCOD home folder (`fs_homepath`):

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

## Checks

`scripts/tests/test_linux_client_core.py` runs the client's connect command and
resend loop against key files: a valid made-up key starts the connection and
sends the GUID request; a mistyped, malformed or group-readable file is
refused; the key never appears in the request or on the console. It also
checks SHA-256 and the CRC-16 check value.
