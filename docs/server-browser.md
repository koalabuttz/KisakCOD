# Finding servers

How a KisakCOD player finds a server to join, and what the game contacts while
looking. In short: the local list, favourites and `connect` work with no setup;
the internet list is off until someone names a master server, because retail's
is gone and this port contacts nobody it was not told to.

## The three ways that need no setup

| Way | Where | What it sends |
| --- | --- | --- |
| Local list | Join Game, source **Local** (console: `localservers`) | A `getinfo` broadcast on the local network, ports 28960 to 28963 |
| Favourites | Join Game, source **Favourites**; add a server from the list or by address | `getinfo` to the addresses the player saved |
| Direct | Console: `connect <address>` or `connect <address>:<port>` | The normal connect handshake to that address |

The local list only sees servers on the same network segment that listen on
ports 28960 to 28963. Favourites are kept in `servercache.dat` in the home
folder. A server on another network is reached by address, or by adding it to
the favourites once.

The client speaks protocol 1 and drops replies for another protocol, so these
lists show KisakCOD servers (and servers started with
[`sv_advertisedProtocol 1`](linux-dedicated.md#server-browser-listing-and-sv_advertisedprotocol)).

## The internet list

Retail asked `cod4master.activision.com` for the list of internet games, and
dedicated servers sent that host a heartbeat every three minutes. That name no
longer resolves. The master is therefore a setting, and it is empty by default:

| Dvar | Default | Meaning |
| --- | --- | --- |
| `masterServerName` | empty | The master, as `host` or `host:port`. Empty: no master is contacted |
| `masterPort` | 20810 | Port used when `masterServerName` has no `:port` |

Both are saved in the player's config once set (`seta`), and work from the
command line (`+set masterServerName master.example`).

With `masterServerName` empty:

- the client's Internet source sends nothing; the console says
  `No master server is set, so there is no internet game list.`
- a dedicated server sends no heartbeat, no shutdown notice and no end-of-game
  status, and looks no name up.

With it set:

- **Client.** Refreshing the Internet source sends `getservers 1 full empty` to
  the master, then `getinfo` to each address the master returns.
- **Server.** A `dedicated 2` server sends the master `heartbeat COD-4` when it
  starts and every three minutes, `heartbeat flatline` on shutdown, and its
  status when the `gameCompleteStatus` command runs, as retail did. The master answers a heartbeat with
  `getinfo <challenge>`, which the server answers like any other `getinfo`.
  Listen servers and `dedicated 1` (LAN) servers never contact the master.
  A server that reports to a master always includes `clients` in its
  `getinfo` reply (retail leaves it out when the server is empty; masters such
  as dpmaster refuse a reply without it).

The server looks the master's name up once. A changed setting is looked up at
once, a failed lookup is retried every 15 minutes, and `heartbeat` in the
server console sends one immediately.

Being listed by a master does not change who may join: admission is still
Steam or `sv_allowUnverifiedGuids`, exactly as without a master.

### What the client accepts from a master

Retail accepted a server list from any address at any time. The client now

- takes a `getserversResponse` only from the master it asked, and only within
  10 seconds of asking;
- reads the list without reading past the end of the packet (retail looked up
  to three bytes further), and lists an address whose bytes happen to spell
  `EOT` (69.79.84.x), which retail took for the end of the list;
- drops entries with port 0, 0.x.x.x addresses, and multicast, reserved and
  broadcast addresses (224.0.0.0 and up). Private and loopback addresses are
  kept so a master on the LAN or on the same machine works;
- refuses changes to `masterServerName` and `masterPort` while connected to a
  remote server. Game servers can set client dvars; they cannot choose the
  master.

### Running a master

There is no public KisakCOD master. Two ways to have one:

**For a test on one machine or LAN**, the repository has a small one:

```sh
python3 scripts/tools/local_master_server.py --port 20810        # this machine only
python3 scripts/tools/local_master_server.py --bind 192.168.1.10  # reachable on the LAN
```

Start a dedicated server with `+set dedicated 2 +set masterServerName 127.0.0.1`
and a client with `+set masterServerName 127.0.0.1`; the server appears under
Join Game, Internet. It is a test tool with no flood protection: do not put it
on the public internet.

**For a public list**, the wire format is the Quake 3 master protocol, which
[dpmaster](https://github.com/kphillisjr/dpmaster) speaks. Reading its source
(`messages.c`, `games.c`), a CoD4 game entry needs the heartbeat tags and the
protocol number declared, along the lines of
`dpmaster --game-properties CoD4 protocols=1 heartbeat=COD-4 flatline=flatline`.
This has not been run: nobody has tried KisakCOD against dpmaster yet.

Whoever runs a master sees the address of every server that reports to it and
of every client that asks it for the list.

## What is checked

Asset-free, on hosted CI:

- `scripts/tests/test_master_server.py` compiles the rules in
  `src/qcommon/master_server.h` 64- and 32-bit with sanitizers (list parsing
  from exact-size buffers, who may answer, when a server reports), checks that
  `masterServerName` is registered empty, and drives the local test master over
  loopback, reading its lists back with the production parser.
- `KisakCOD-client-server-list-test` (part of the
  [client core](linux-client.md) targets) runs the unmodified `globalservers`
  and `localservers` commands, the list and `infoResponse` handlers and the
  server's heartbeat code through the engine's own UDP socket against sockets
  on 127.0.0.1: nothing is sent without a master, retail's packets go to a set
  master, a list from anyone else is ignored, and a connected client's master
  settings cannot be changed.

Not yet checked: the Join Game menu itself on a real client, and a real
dedicated server, test master and client together. Both need game assets.
