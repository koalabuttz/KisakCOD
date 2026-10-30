#!/usr/bin/env python3
"""Opt-in real UDP handshake checks using legally installed COD4 assets.

Exercises connection admission, not gamestate decoding or playable-client
compatibility. Assets are symlinked read-only into a separate temporary home.
"""
import argparse
from pathlib import Path
import re
import select
import socket
import subprocess
import tempfile
import time

from test_linux_server_lifecycle import CheckFailure, OOB, Server

ROOT = Path(__file__).resolve().parents[2]


def require(condition, message):
    if not condition:
        raise CheckFailure(message)
    print("PASS: " + message, flush=True)


def exchange(server, sock, text):
    sock.sendto(OOB + text.encode("ascii") + b"\n", server.address)
    deadline = time.monotonic() + 5
    while time.monotonic() < deadline:
        server.alive()
        try:
            packet, source = sock.recvfrom(65535)
        except socket.timeout:
            continue
        if source == server.address and packet.startswith(OOB):
            reply = packet[len(OOB):].decode("ascii", "replace").rstrip("\x00\n")
            if reply.startswith(("infoResponse\n", "statusResponse\n")):
                continue  # Delayed readiness-query datagram, not a handshake.
            return reply
    raise CheckFailure("no handshake reply to " + text[:60])


def console_barrier(server, commands):
    marker = "map: mp_crash\n"
    offset = server.log_path.stat().st_size
    for command in commands:
        server.send(command)
    server.send("status")
    deadline = time.monotonic() + server.timeout
    while time.monotonic() < deadline:
        server.alive()
        if marker in server.log_path.read_bytes()[offset:].decode(errors="replace"):
            return
        select.select([], [], [], 0.05)
    raise CheckFailure("console barrier timed out")


def connect(token, protocol=1, qport=19341, password=""):
    return (f'connect "\\protocol\\{protocol}\\challenge\\{token}\\qport\\{qport}'
            f'\\name\\HandshakeProbe\\rate\\25000\\snaps\\20\\password\\{password}"')


def run_session(binary, game, work, enabled):
    args = ["+set", "sv_loopbackTestAuth", str(int(enabled))]
    server = Server(binary, game, work, work, 60, startup_args=args)
    try:
        server.query("getinfo", "mp_crash")  # Nonce/name-correlated readiness.
        sock = server.socket
        if not enabled:
            for request, expected in (
                ("getchallenge 123", "custom Steam ticket/ID handshake"),
                ("getchallenge 123 !!!! 1", "Malformed Steam"),
                ("getchallenge 123 AA== local-test-forged", "Malformed Steam"),
                ("getchallenge 123 AA== 18446744073709551616", "Malformed Steam"),
                ("getchallenge 123 AA== 1", "Steam authentication is unavailable"),
            ):
                reply = exchange(server, sock, request)
                require(expected in reply, "default mode: " + expected + "; reply=" + repr(reply))
            require("BAD_CHALLENGE" in exchange(server, sock, connect(123)),
                    "default mode cannot connect after rejected authentication")
        else:
            reply = exchange(server, sock, "getchallenge 123")
            match = re.fullmatch(r"challengeResponse (-?\d+)", reply)
            require(match is not None, "startup opt-in issues a challenge without Steam fields")
            token = int(match[1])
            require(exchange(server, sock, "getchallenge 456") == reply, "challenge retry retains token")
            for protocol in (6, 21):
                rejection = exchange(server, sock, connect(token, protocol=protocol))
                require(rejection == f"error\nThis server requires protocol version 1 (CoD4 1.0); "
                                 f"your client uses protocol {protocol}.",
                        f"client protocol {protocol} gets an explicit error independently of authentication")
            require("BAD_CHALLENGE" in exchange(server, sock, connect(token ^ 1)), "wrong token rejected")
            with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as other:
                other.bind(("127.0.0.1", 0))
                other.settimeout(0.25)
                require("BAD_CHALLENGE" in exchange(server, other, connect(token)), "changed source port rejected")
            with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as other:
                other.bind(("127.0.0.2", 0))
                other.settimeout(0.25)
                require("requires source and server bind 127.0.0.1" in exchange(server, other, "getchallenge 123"),
                        "127.0.0.2 source rejected by live server")
            console_barrier(server, ["set sv_loopbackTestAuth 0"])
            require(exchange(server, sock, "getchallenge 789") == reply, "console cannot change startup-only option")
            console_barrier(server, ["net_restart"])
            server.query("getinfo", "mp_crash")
            require(exchange(server, sock, "getchallenge 789") == reply, "safe net_restart preserves local challenge mode")
            console_barrier(server, ["set g_password LocalProbePassword"])
            require("GAME_INVALIDPASSWORD" in exchange(server, sock, connect(token)),
                    "authentication bypass does not skip the game password")
            reply = exchange(server, sock, connect(token, password="LocalProbePassword"))
            require(reply.startswith("connectResponse"), "protocol 1 with issued challenge and password reaches real connection admission")
            # The wire probe never sends gameplay packets. Verify the admitted
            # identity in the actual game log, then quit to exercise its cleanup.
            require(re.search(r'guid "local-test-[0-9a-f]{12}"', server.log()) is not None,
                    "admitted client has a server-generated non-Steam identity")
        server.shutdown("console")
    finally:
        server.cleanup()


def startup_matrix(binary, game, work):
    assets = work / "assets"
    assets.mkdir()
    for name in ("main", "zone"):
        (assets / name).symlink_to(game / name, target_is_directory=True)
    for index, bind in enumerate((None, "", "localhost", "0.0.0.0", "127.0.0.2", "192.0.2.1", "example.invalid")):
        home = work / str(index)
        home.mkdir()
        (home / "localization.txt").symlink_to(game / "localization.txt")
        command = [str(binary), "+set", "fs_basepath", str(assets), "+set", "fs_homepath", str(home),
                   "+set", "dedicated", "1", "+set", "sv_loopbackTestAuth", "1", "+set", "net_noudp", "1"]
        if bind is not None:
            command += ["+set", "net_ip", bind]
        command += ["+quit"]
        result = subprocess.run(command, cwd=home, stdin=subprocess.DEVNULL, stdout=subprocess.PIPE,
                                stderr=subprocess.STDOUT, text=True, errors="replace", timeout=60)
        require(result.returncode != 0 and "must be exactly" in result.stdout,
                f"unsafe startup bind {bind!r} fails even with UDP disabled")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("game_dir", type=Path)
    parser.add_argument("--binary", type=Path, default=ROOT / "bin/KisakCOD-dedi")
    args = parser.parse_args()
    binary, game = args.binary.resolve(), args.game_dir.expanduser().resolve()
    if not binary.is_file() or not all((game / name).exists() for name in ("main", "zone", "localization.txt")):
        parser.error("requires a built native server and installed main/zone/localization.txt")
    with tempfile.TemporaryDirectory(prefix="kisak-handshake-") as temporary:
        work = Path(temporary)
        try:
            for label, enabled in (("default", False), ("local", True)):
                session = work / label
                session.mkdir()
                run_session(binary, game, session, enabled)
            matrix = work / "matrix"
            matrix.mkdir()
            startup_matrix(binary, game, matrix)
        except (CheckFailure, OSError, subprocess.TimeoutExpired) as error:
            print("FAIL:", error, flush=True)
            for log in work.glob("*/server.log"):
                print(str(log) + "\n" + log.read_text(errors="replace")[-12000:])
            return 1
    print("Live handshake admission passed; playable stock/CoD4x compatibility is NOT established.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
