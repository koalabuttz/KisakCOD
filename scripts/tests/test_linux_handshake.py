#!/usr/bin/env python3
"""Opt-in real UDP handshake checks using legally installed COD4 assets.

Exercises connection admission, not gamestate decoding or playable-client
compatibility. Assets are symlinked read-only into a separate temporary home.

Three server configurations are started: default authentication, the loopback
local-test opt-in, and the loopback local-test opt-in together with
``sv_allowProtocol6`` so the retail 1.7 protocol number (6) is admitted through
the protocol-1 path. A fourth start covers ``sv_allowProtocol6`` on its own,
which must still refuse every client because authentication is unchanged.

Five more starts cover the retail GUID opt-in itself, which is independent of the
loopback test bypass and never turns it on:

  * ``sv_allowUnverifiedGuids 1`` with a real ``getchallenge 0 "<32 hex GUID>"``
    exchange and a protocol 1 connect;
  * the same with ``sv_allowProtocol6 1`` and a protocol 6 connect, i.e. the
    retail 1.7 hosting configuration;
  * the same bound to a local non-loopback address, connecting from it, which
    proves no source address is special-cased;
  * a default server, where ``set sv_allowUnverifiedGuids 1`` at the console must
    not enable anything because the option is read once at start-up;
  * ``sv_loopbackTestAuth 1`` and ``sv_allowUnverifiedGuids 1`` together, which
    must stop start-up before anything binds.

Five further starts cover ``sv_advertisedProtocol``, which changes only what
getinfo and getstatus report, never admission:

  * a default start, where both browser queries report protocol 1 and a console
    ``set sv_advertisedProtocol 6`` is refused as write protected;
  * ``+set sv_advertisedProtocol 6`` with ``+set sv_allowProtocol6 1``, where both
    queries report 6 and a protocol 6 client still reaches admission;
  * ``+set sv_advertisedProtocol 3``, which is neither supported value and must
    warn and fall back to 1;
  * the same with ``9``, which is outside any useful range and must not be
    saturated to 6 by the dvar domain instead of falling back;
  * ``+set sv_advertisedProtocol 6`` alone, which must warn that 1.7 clients will
    see the server but cannot join, while a 1.0 client is still admitted.
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


# The retail CoD4 1.7 GUID observed from a real client, and the same value in
# uppercase to prove the server normalises the spelling.
RETAIL_GUID = "9fa4b618b026886e7fe966bb184ae770"
RETAIL_GUID_UPPER = RETAIL_GUID.upper()

MALFORMED_GUID_ERROR = "error\nMalformed GUID authentication fields."
GUID_DISABLED_ERROR = ("error\nThis server requires Steam authentication; "
                       "unverified retail GUIDs are not accepted.")


def retail_getchallenge(guid=RETAIL_GUID):
    return f'getchallenge 0 "{guid}"'


def local_non_loopback_address():
    """A local IPv4 address that is not 127.0.0.1, or None when there is none."""
    try:
        with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as probe:
            # TEST-NET-1 is never routed, so this only asks the kernel which local
            # address the default route would use; no packet is sent.
            probe.connect(("192.0.2.1", 9))
            address = probe.getsockname()[0]
    except OSError:
        return None
    return None if address.startswith("127.") else address


def rebind_client_to(server, address):
    """Point the test client at a server bound to a specific local address."""
    port = server.address[1]
    server.socket.close()
    server.socket = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    server.socket.bind((address, 0))
    server.socket.settimeout(0.25)
    server.address = (address, port)


def run_session(binary, game, work, enabled, allow_protocol6=False):
    args = ["+set", "sv_loopbackTestAuth", str(int(enabled))]
    if allow_protocol6:
        args += ["+set", "sv_allowProtocol6", "1"]
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
            # With the compatibility switch off, protocol 6 is refused like any
            # other version; with it on, 6 is admitted and the error text names
            # the accepted alternatives.
            suffix = " or 6 (CoD4 1.7)" if allow_protocol6 else ""
            admitted = 6 if allow_protocol6 else 1
            for protocol in (7, 21) if allow_protocol6 else (6, 21):
                rejection = exchange(server, sock, connect(token, protocol=protocol))
                require(rejection == f"error\nThis server requires protocol version 1 (CoD4 1.0){suffix}; "
                                 f"your client uses protocol {protocol}.",
                        f"client protocol {protocol} gets an explicit error independently of authentication")
            require("BAD_CHALLENGE" in exchange(server, sock, connect(token ^ 1, protocol=admitted)),
                    "wrong token rejected")
            with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as other:
                other.bind(("127.0.0.1", 0))
                other.settimeout(0.25)
                require("BAD_CHALLENGE" in exchange(server, other, connect(token, protocol=admitted)),
                        "changed source port rejected")
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
            # A wide reconnect window keeps the retry below inside it whatever the
            # machine speed, so this probe does not depend on how fast the server
            # started.
            console_barrier(server, ["set sv_reconnectlimit 600"])
            require("GAME_INVALIDPASSWORD" in exchange(server, sock, connect(token, protocol=admitted)),
                    "authentication bypass does not skip the game password")
            # That rejected attempt left its client slot free but already carrying
            # this address and connect port. A free slot is not a reconnect, so
            # SV_DirectConnect must admit the corrected retry at once instead of
            # throttling it as a too-soon reconnect of an admitted client.
            reply = exchange(server, sock, connect(token, protocol=admitted, password="LocalProbePassword"))
            require(reply.startswith("connectResponse"),
                    f"protocol {admitted} with issued challenge and password reaches real connection admission "
                    f"immediately after a rejection inside sv_reconnectlimit")
            # The wire probe never sends gameplay packets. Verify the admitted
            # identity in the actual game log, then quit to exercise its cleanup.
            require(re.search(r'guid "local-test-[0-9a-f]{12}"', server.log()) is not None,
                    "admitted client has a server-generated non-Steam identity")
        server.shutdown("console")
    finally:
        server.cleanup()


def protocol6_without_auth(binary, game, work):
    """sv_allowProtocol6 alone: the version check changes, authentication does not."""
    server = Server(binary, game, work, work, 60,
                    startup_args=["+set", "sv_loopbackTestAuth", "0", "+set", "sv_allowProtocol6", "1"])
    try:
        server.query("getinfo", "mp_crash")
        sock = server.socket
        reply = exchange(server, sock, "getchallenge 123")
        require("Steam ticket/ID handshake" in reply,
                "protocol 6 switch leaves the default authentication requirement: " + repr(reply))
        reply = exchange(server, sock, connect(123, protocol=6))
        require("BAD_CHALLENGE" in reply,
                "protocol 6 without an authenticated challenge is still refused: " + repr(reply))
        server.shutdown("console")
    finally:
        server.cleanup()


VALUE_WARNING = "sv_advertisedProtocol must be 1 or 6; advertising protocol 1"
MISMATCH_WARNING = "sv_advertisedProtocol is 6 but sv_allowProtocol6 is off"


def admitted_reply(server, protocol):
    """Reach real connection admission once, so a browsing change is proven inert."""
    sock = server.socket
    reply = exchange(server, sock, "getchallenge 123")
    match = re.fullmatch(r"challengeResponse (-?\d+)", reply)
    require(match is not None,
            f"a server advertising protocol {server.protocol} still issues challenges: " + repr(reply))
    return exchange(server, sock, connect(int(match[1]), protocol=protocol))


def advertised_default(binary, game, work):
    """A stock start advertises protocol 1 to both browser queries and cannot change it."""
    server = Server(binary, game, work, work, 60,
                    startup_args=["+set", "sv_loopbackTestAuth", "1"])
    try:
        # query() rejects any reply whose protocol is not self.protocol, so these
        # two calls are the assertion that both queries report 1.
        server.query("getinfo", "mp_crash")
        server.query("getstatus", "mp_crash")
        require(VALUE_WARNING not in server.log() and MISMATCH_WARNING not in server.log(),
                "a default start prints no sv_advertisedProtocol warning")
        # The advertised value is read once at start-up, like the other
        # startup-only options, so a live console set must be a no-op.
        console_barrier(server, ["set sv_advertisedProtocol 6"])
        require("sv_advertisedProtocol is write protected" in server.log(),
                "the console cannot change the advertised protocol at run time")
        server.query("getinfo", "mp_crash")
        server.query("getstatus", "mp_crash")
        require(admitted_reply(server, 1).startswith("connectResponse"),
                "a default server still admits a protocol 1 client")
        server.shutdown("console")
    finally:
        server.cleanup()


def advertised_protocol6(binary, game, work):
    """Advertising 6 with sv_allowProtocol6 on reports 6 and still admits 1.7 clients."""
    server = Server(binary, game, work, work, 60, protocol="6",
                    startup_args=["+set", "sv_loopbackTestAuth", "1",
                                  "+set", "sv_allowProtocol6", "1",
                                  "+set", "sv_advertisedProtocol", "6"])
    try:
        server.query("getinfo", "mp_crash")
        server.query("getstatus", "mp_crash")
        require(VALUE_WARNING not in server.log(),
                "6 is an accepted value and does not fall back")
        require(MISMATCH_WARNING not in server.log(),
                "6 with sv_allowProtocol6 on prints no mismatch warning")
        require(admitted_reply(server, 6).startswith("connectResponse"),
                "advertising 6 leaves the 1.7 admission path working")
        # The answer is captured once per process, so the socket restart and a real
        # map change must not lose or change it.  transition() waits for the fresh
        # console "Server:" marker, so a status line from the old map cannot pass.
        console_barrier(server, ["net_restart"])
        server.query("getinfo", "mp_crash")
        server.query("getstatus", "mp_crash")
        server.transition("advertised6 restart", "map_restart", "mp_crash", 0)
        server.transition("advertised6 map", "map mp_shipment", "mp_shipment", 0)
        require(VALUE_WARNING not in server.log(),
                "a socket restart and two map reloads do not re-validate the captured protocol")
        server.shutdown("console")
    finally:
        server.cleanup()


def advertised_invalid_value(binary, game, work, value="3"):
    """A value that is neither 1 nor 6 warns and falls back to advertising 1.

    Both an in-limits odd value (3) and one outside any plausible range (9) are
    checked: the second is the case where a registered 1..6 domain would have
    saturated silently to 6 instead of warning, which is the dangerous direction.
    """
    server = Server(binary, game, work, work, 60,
                    startup_args=["+set", "sv_loopbackTestAuth", "1",
                                  "+set", "sv_advertisedProtocol", value])
    try:
        # The expected protocol stays at the Server default, so these queries fail
        # if the unsupported value was advertised instead of replaced.
        server.query("getinfo", "mp_crash")
        server.query("getstatus", "mp_crash")
        require(VALUE_WARNING in server.log(),
                f"the unsupported value {value} warns that it is being replaced with 1")
        require(MISMATCH_WARNING not in server.log(),
                f"the unsupported value {value} never falls through to the 1.7 warning")
        server.shutdown("console")
    finally:
        server.cleanup()


def advertised_without_protocol6(binary, game, work):
    """Advertising 6 without sv_allowProtocol6 warns but still admits 1.0 clients."""
    server = Server(binary, game, work, work, 60, protocol="6",
                    startup_args=["+set", "sv_loopbackTestAuth", "1",
                                  "+set", "sv_advertisedProtocol", "6"])
    try:
        server.query("getinfo", "mp_crash")
        server.query("getstatus", "mp_crash")
        require(MISMATCH_WARNING in server.log(),
                "advertising 6 with the 1.7 switch off names the consequence in the log")
        require(VALUE_WARNING not in server.log(),
                "6 is accepted even when 1.7 clients cannot join")
        # The warning's other half: a 1.7 client really is refused, and the error
        # text names only version 1 because sv_allowProtocol6 is off.
        sock = server.socket
        reply = exchange(server, sock, "getchallenge 123")
        match = re.fullmatch(r"challengeResponse (-?\d+)", reply)
        require(match is not None, "advertising 6 without sv_allowProtocol6 still issues challenges")
        require(exchange(server, sock, connect(int(match[1]), protocol=6)) ==
                "error\nThis server requires protocol version 1 (CoD4 1.0); "
                "your client uses protocol 6.",
                "advertising 6 does not admit a protocol 6 client while the switch is off")
        # Advertising is not admission: the published protocol number alone must
        # never lock out the clients the server does accept.
        require(admitted_reply(server, 1).startswith("connectResponse"),
                "advertising 6 does not stop a protocol 1 client from joining")
        server.shutdown("console")
    finally:
        server.cleanup()


def run_guid_session(binary, game, work, allow_protocol6=False, bind_address=None):
    """A GUID-on server with no loopback test mode: the retail hosting case."""
    prefix = f"[guids{'/proto6' if allow_protocol6 else ''}{'/'+bind_address if bind_address else ''}] "
    args = ["+set", "sv_allowUnverifiedGuids", "1"]
    if allow_protocol6:
        args += ["+set", "sv_allowProtocol6", "1"]
    if bind_address:
        args += ["+set", "net_ip", bind_address]
    server = Server(binary, game, work, work, 60, startup_args=args)
    try:
        if bind_address:
            rebind_client_to(server, bind_address)
        server.query("getinfo", "mp_crash")
        require("sv_allowUnverifiedGuids is enabled" in server.log(),
                prefix + "the opt-in printed its warning")
        require("sv_loopbackTestAuth is enabled" not in server.log(),
                prefix + "the loopback test bypass stayed off")

        sock = server.socket
        reply = exchange(server, sock, retail_getchallenge())
        match = re.fullmatch(r"challengeResponse (-?\d+)", reply)
        require(match is not None, prefix + "a retail GUID request issues a challenge: " + repr(reply))
        token = int(match[1])

        require(exchange(server, sock, retail_getchallenge(RETAIL_GUID_UPPER)) == reply,
                prefix + "the uppercase spelling keeps the same challenge")

        # A malformed GUID must invalidate the token that was already issued for
        # this address, so a failed retry cannot be replayed.
        bad = exchange(server, sock, retail_getchallenge(RETAIL_GUID[:-1]))
        require(bad == MALFORMED_GUID_ERROR, prefix + "a 31-character GUID is refused: " + repr(bad))
        require("BAD_CHALLENGE" in exchange(server, sock, connect(token)),
                prefix + "the malformed retry invalidated the earlier token")

        # Ask again, then connect for real.
        reply = exchange(server, sock, retail_getchallenge())
        match = re.fullmatch(r"challengeResponse (-?\d+)", reply)
        require(match is not None, prefix + "a fresh challenge was issued: " + repr(reply))
        token = int(match[1])
        admitted = 6 if allow_protocol6 else 1
        require("BAD_CHALLENGE" in exchange(server, sock, connect(token ^ 1, protocol=admitted)),
                prefix + "a wrong token is still rejected")
        reply = exchange(server, sock, connect(token, protocol=admitted))
        require(reply.startswith("connectResponse"),
                prefix + f"protocol {admitted} with a retail GUID reaches connection admission: " + repr(reply))
        require(re.search(r'guid "' + RETAIL_GUID + r'"', server.log()) is not None,
                prefix + "the admitted client carries the claimed retail GUID")

        # The opt-in is startup-only: the console cannot change it.
        console_barrier(server, ["set sv_allowUnverifiedGuids 0"])
        require("sv_allowUnverifiedGuids is write protected" in server.log(),
                prefix + "the option is write protected at run time")
        reply = exchange(server, sock, retail_getchallenge())
        require(re.fullmatch(r"challengeResponse (-?\d+)", reply) is not None,
                prefix + "the console cannot revoke the start-up decision: " + repr(reply))
        server.shutdown("console")
    finally:
        server.cleanup()


def guid_off_session(binary, game, work):
    """Default server: retail GUIDs are refused and the console cannot enable them."""
    server = Server(binary, game, work, work, 60,
                    startup_args=["+set", "sv_loopbackTestAuth", "0"])
    try:
        server.query("getinfo", "mp_crash")
        sock = server.socket
        reply = exchange(server, sock, retail_getchallenge())
        require(reply == GUID_DISABLED_ERROR,
                "default mode refuses a retail GUID: " + repr(reply))
        require("BAD_CHALLENGE" in exchange(server, sock, connect(123)),
                "default mode cannot connect on a GUID it refused")

        console_barrier(server, ["set sv_allowUnverifiedGuids 1"])
        require("sv_allowUnverifiedGuids is write protected" in server.log(),
                "the console cannot set the startup-only option")
        reply = exchange(server, sock, retail_getchallenge())
        require(reply == GUID_DISABLED_ERROR,
                "a runtime set does not let a default server accept retail GUIDs: " + repr(reply))
        server.shutdown("console")
    finally:
        server.cleanup()


def both_switches_are_fatal(binary, game, work):
    """sv_loopbackTestAuth plus sv_allowUnverifiedGuids must stop start-up."""
    assets = work / "assets"
    assets.mkdir()
    for name in ("main", "zone"):
        (assets / name).symlink_to(game / name, target_is_directory=True)
    home = work / "home"
    home.mkdir()
    (home / "localization.txt").symlink_to(game / "localization.txt")
    command = [str(binary), "+set", "fs_basepath", str(assets), "+set", "fs_homepath", str(home),
               "+set", "dedicated", "1", "+set", "net_ip", "127.0.0.1", "+set", "net_noudp", "1",
               "+set", "sv_loopbackTestAuth", "1", "+set", "sv_allowUnverifiedGuids", "1",
               "+map", "mp_crash"]
    try:
        result = subprocess.run(command, cwd=home, stdin=subprocess.DEVNULL, stdout=subprocess.PIPE,
                                stderr=subprocess.STDOUT, text=True, errors="replace", timeout=45)
    except subprocess.TimeoutExpired:
        raise CheckFailure("both switches did not stop start-up; the server kept running")
    require(result.returncode != 0,
            "both switches make the process exit non-zero instead of serving")
    require("sv_loopbackTestAuth and sv_allowUnverifiedGuids cannot both be enabled" in result.stdout,
            "the failure names both switches")


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
            for label, enabled, allow_protocol6 in (("default", False, False),
                                                    ("local", True, False),
                                                    ("local-protocol6", True, True)):
                session = work / label
                session.mkdir()
                run_session(binary, game, session, enabled, allow_protocol6)
            session = work / "protocol6-no-auth"
            session.mkdir()
            protocol6_without_auth(binary, game, session)
            for label, scenario, kwargs in (("advertised-default", advertised_default, {}),
                                            ("advertised-protocol6", advertised_protocol6, {}),
                                            ("advertised-invalid", advertised_invalid_value,
                                             {"value": "3"}),
                                            ("advertised-invalid-high", advertised_invalid_value,
                                             {"value": "9"}),
                                            ("advertised-without-protocol6",
                                             advertised_without_protocol6, {})):
                session = work / label
                session.mkdir()
                scenario(binary, game, session, **kwargs)
            for label, allow_protocol6, bind_address in (("guids", False, None),
                                                         ("guids-protocol6", True, None),
                                                         ("guids-off", False, None)):
                session = work / label
                session.mkdir()
                if label == "guids-off":
                    guid_off_session(binary, game, session)
                else:
                    run_guid_session(binary, game, session, allow_protocol6, bind_address)
            address = local_non_loopback_address()
            if address is None:
                print("SKIP: no non-127.0.0.1 local address; the any-source GUID check is not run",
                      flush=True)
            else:
                session = work / "guids-nonloopback"
                session.mkdir()
                run_guid_session(binary, game, session, bind_address=address)
            session = work / "both-switches"
            session.mkdir()
            both_switches_are_fatal(binary, game, session)
            matrix = work / "matrix"
            matrix.mkdir()
            startup_matrix(binary, game, matrix)
        except (CheckFailure, OSError, subprocess.TimeoutExpired) as error:
            print("FAIL:", error, flush=True)
            for log in work.glob("*/server.log"):
                print(str(log) + "\n" + log.read_text(errors="replace")[-12000:])
            return 1
    print("Live handshake admission passed, including optional protocol 6 admission for local-test "
          "authentication and the opt-in retail GUID path; gamestate decoding, real Steam "
          "authentication and stock/CoD4x playable compatibility are NOT established here.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
