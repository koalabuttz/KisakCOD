#!/usr/bin/env python3
"""A small master server for testing KisakCOD's internet server list locally.

KisakCOD contacts no master server unless ``masterServerName`` is set (see
``docs/server-browser.md``).  This tool is the other end for a test on one
machine or one LAN: point a dedicated server and a client at it and the
client's Join Game > Internet list shows the server.

    python3 scripts/tools/local_master_server.py --port 20810
    KisakCOD-dedi +set dedicated 2 +set masterServerName 127.0.0.1 ...
    KisakCOD-mp   +set masterServerName 127.0.0.1 ...

It speaks the Quake 3 master protocol retail CoD4 used, with the registration
rules of dpmaster (read from its source, messages.c):

  server -> master   ``heartbeat COD-4``          (``heartbeat flatline`` on shutdown)
  master -> server   ``getinfo <challenge>``
  server -> master   ``infoResponse`` + ``\\challenge\\..\\protocol\\..\\sv_maxclients\\..\\clients\\..``
  client -> master   ``getservers <protocol> [full] [empty]``
  master -> client   ``getserversResponse`` + ``\\`` + 4 address bytes + 2 port bytes ... ``\\EOT``

A server is listed only after it answers the challenge from the address its
heartbeat came from, and is dropped when it stops sending heartbeats.

This is a test tool.  It binds 127.0.0.1 unless told otherwise, has no flood
protection, and must not be put on the public internet; run dpmaster or a
similar hardened master for that.
"""

import argparse
import secrets
import select
import socket
import sys
import time

OOB = b"\xff\xff\xff\xff"
CHALLENGE_TIMEOUT = 2.0        # seconds a server has to answer getinfo
SERVER_TIMEOUT = 2 * 180 + 30  # two missed three-minute heartbeats
MAX_SERVERS = 256
MAX_PACKET_ENTRIES = 100       # keeps each reply well under one UDP datagram


def parse_info(text):
    """Parses ``\\key\\value\\key\\value`` into a dict; None when malformed."""
    if not text.startswith("\\"):
        return None
    parts = text[1:].split("\\")
    if len(parts) % 2:
        return None
    return dict(zip(parts[0::2], parts[1::2]))


class Master:
    def __init__(self, sock, log=None, now=time.monotonic):
        self.sock = sock
        self.log = log or (lambda message: None)
        self.now = now
        self.pending = {}  # address -> (challenge, deadline)
        self.servers = {}  # address -> {"protocol", "clients", "maxclients", "expires"}

    # -- packets from game servers ---------------------------------------------

    def heartbeat(self, address, tag):
        if tag == "flatline":
            if self.servers.pop(address, None) is not None:
                self.log(f"{address[0]}:{address[1]} went offline")
            self.pending.pop(address, None)
            return
        if address not in self.servers and len(self.servers) + len(self.pending) >= MAX_SERVERS:
            self.log("server table full; heartbeat ignored")
            return
        challenge = secrets.token_hex(6)
        self.pending[address] = (challenge, self.now() + CHALLENGE_TIMEOUT)
        self.sock.sendto(OOB + b"getinfo " + challenge.encode(), address)

    def info_response(self, address, body):
        pending = self.pending.get(address)
        info = parse_info(body)
        if pending is None or info is None:
            return
        challenge, deadline = pending
        if self.now() > deadline or info.get("challenge") != challenge:
            self.log(f"{address[0]}:{address[1]} answered with a wrong or late challenge")
            return
        try:
            protocol = int(info["protocol"])
            maxclients = int(info["sv_maxclients"])
            clients = int(info["clients"])  # dpmaster refuses a reply without it
        except (KeyError, ValueError):
            self.log(f"{address[0]}:{address[1]} sent an infoResponse without protocol, sv_maxclients or clients")
            return
        if maxclients <= 0 or clients < 0:
            return
        del self.pending[address]
        if address not in self.servers:
            self.log(f"{address[0]}:{address[1]} registered (protocol {protocol}, {clients}/{maxclients})")
        self.servers[address] = {
            "protocol": protocol,
            "clients": clients,
            "maxclients": maxclients,
            "expires": self.now() + SERVER_TIMEOUT,
        }

    # -- packets from clients --------------------------------------------------

    def get_servers(self, address, arguments):
        try:
            protocol = int(arguments[0])
        except (IndexError, ValueError):
            return
        want_full = "full" in arguments[1:]
        want_empty = "empty" in arguments[1:]
        self.expire()
        listed = []
        for (host, port), server in self.servers.items():
            if server["protocol"] != protocol:
                continue
            if server["clients"] == 0 and not want_empty:
                continue
            if server["clients"] >= server["maxclients"] and not want_full:
                continue
            listed.append(socket.inet_aton(host) + port.to_bytes(2, "big"))
        self.log(f"{address[0]}:{address[1]} asked for protocol {protocol}: {len(listed)} listed")
        for start in range(0, max(len(listed), 1), MAX_PACKET_ENTRIES):
            packet = OOB + b"getserversResponse"
            for entry in listed[start:start + MAX_PACKET_ENTRIES]:
                packet += b"\\" + entry
            self.sock.sendto(packet + b"\\EOT\0\0\0", address)

    # -- plumbing --------------------------------------------------------------

    def expire(self):
        now = self.now()
        for address in [a for a, server in self.servers.items() if server["expires"] < now]:
            del self.servers[address]
            self.log(f"{address[0]}:{address[1]} timed out")
        for address in [a for a, (_, deadline) in self.pending.items() if deadline < now]:
            del self.pending[address]

    def handle(self, data, address):
        if not data.startswith(OOB) or len(data) > 2048:
            return
        text = data[4:].decode("latin-1")
        if text.startswith("heartbeat "):
            words = text.split()
            self.heartbeat(address, words[1] if len(words) > 1 else "")
        elif text.startswith("infoResponse\n"):
            self.info_response(address, text[len("infoResponse\n"):].rstrip("\0"))
        elif text.startswith("getservers "):
            self.get_servers(address, text[len("getservers "):].split())

    def serve(self, should_stop=lambda: False):
        while not should_stop():
            readable, _, _ = select.select([self.sock], [], [], 0.2)
            if readable:
                try:
                    data, address = self.sock.recvfrom(4096)
                except OSError:
                    continue
                self.handle(data, address)
            self.expire()


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--bind", default="127.0.0.1",
                        help="address to listen on (default 127.0.0.1; use a LAN address to test across machines)")
    parser.add_argument("--port", type=int, default=20810, help="UDP port (default 20810, the masterPort default)")
    args = parser.parse_args()

    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    sock.bind((args.bind, args.port))
    print(f"local test master listening on {args.bind}:{sock.getsockname()[1]} (Ctrl+C stops it)", flush=True)
    master = Master(sock, log=lambda message: print(message, flush=True))
    try:
        master.serve()
    except KeyboardInterrupt:
        pass
    return 0


if __name__ == "__main__":
    sys.exit(main())
