#!/usr/bin/env python3
"""Check the rules for the internet server list ("master server").

``src/qcommon/master_server.h`` holds the header-only rules that decide who a
KisakCOD client or server contacts to list internet games, and what a client
accepts back:

  ``Master_NameIsSet``             an empty ``masterServerName`` means nobody is
                                   contacted; that is the registered default.
  ``Master_ParseServersResponse``  reads the addresses out of a
                                   ``getserversResponse`` datagram without
                                   reading past its end, and drops addresses a
                                   master must not make the client query.
  ``Master_QueryAccepts``          a list is taken only from the master that was
                                   asked, shortly after asking.
  ``Master_HeartbeatDue`` and
  ``Master_ShouldResolve``         only a ``dedicated 2`` server with a master
                                   set sends heartbeats, and it looks the name
                                   up once, retrying a failure every 15 minutes.

Three things are checked, with no game data and no network beyond 127.0.0.1:

1. The real header is compiled into a small native program, 64-bit and - when
   the toolchain can - 32-bit like the shipped server, with the address and
   undefined-behaviour sanitizers when available, and every rule is exercised.
   Each packet is parsed from a heap block of exactly its size, so a read past
   the end of a datagram fails the run.
2. The production dvar registration in ``src/qcommon/common.cpp`` really has the
   empty default.
3. ``scripts/tools/local_master_server.py``, the local test master, is driven
   over loopback UDP by a scripted game server and client, and the list it
   sends is read back with the production parser from step 1.

``--m32`` makes the 32-bit build a requirement.
"""

import argparse
import os
from pathlib import Path
import re
import shlex
import socket
import subprocess
import sys
import tempfile
import threading

ROOT = Path(__file__).resolve().parents[2]
HEADER = ROOT / "src" / "qcommon" / "master_server.h"
COMMON = ROOT / "src" / "qcommon" / "common.cpp"
sys.path.insert(0, str(ROOT / "scripts" / "tools"))
import local_master_server  # noqa: E402

PROGRAM = r'''
#include "master_server.h"

#include <climits>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

static int failures = 0;

static void expect(bool got, bool want, const char* what)
{
    if (got != want)
    {
        ++failures;
        std::printf("FAIL: %s: got %s, want %s\n", what, got ? "true" : "false", want ? "true" : "false");
    }
}

static void expect_int(int got, int want, const char* what)
{
    if (got != want)
    {
        ++failures;
        std::printf("FAIL: %s: got %d, want %d\n", what, got, want);
    }
}

// Parses from a heap block of exactly the packet's size so the sanitizer sees
// any read outside the datagram.
static int parse(const std::string& packet, std::vector<MasterServerEntry>* out, int maxOut, int* skipped)
{
    uint8_t* exact = static_cast<uint8_t*>(std::malloc(packet.size() ? packet.size() : 1));
    std::memcpy(exact, packet.data(), packet.size());
    out->assign(maxOut > 0 ? maxOut : 1, MasterServerEntry{});
    const int count = Master_ParseServersResponse(
        packet.size() ? exact : nullptr, static_cast<int>(packet.size()), out->data(), maxOut, skipped);
    std::free(exact);
    out->resize(count > 0 ? count : 0);
    return count;
}

static std::string entry(int a, int b, int c, int d, int port)
{
    const char bytes[7] = {'\\', char(a), char(b), char(c), char(d), char(port >> 8), char(port & 0xff)};
    return std::string(bytes, sizeof(bytes));
}

static const std::string header("\xff\xff\xff\xffgetserversResponse", 22);
static const std::string eot("\\EOT\0\0\0", 7);

static bool is(const MasterServerEntry& e, int a, int b, int c, int d, int port)
{
    return e.ip[0] == a && e.ip[1] == b && e.ip[2] == c && e.ip[3] == d
        && ((e.portHigh << 8) | e.portLow) == port;
}

static int print_parsed(const char* hex)
{
    std::string packet;
    for (size_t i = 0; hex[i] && hex[i + 1]; i += 2)
        packet.push_back(static_cast<char>(std::strtol(std::string(hex + i, 2).c_str(), nullptr, 16)));
    std::vector<MasterServerEntry> out;
    int skipped = 0;
    parse(packet, &out, MASTER_MAX_ENTRIES_PER_PACKET, &skipped);
    for (const MasterServerEntry& e : out)
        std::printf("%d.%d.%d.%d:%d\n", e.ip[0], e.ip[1], e.ip[2], e.ip[3], (e.portHigh << 8) | e.portLow);
    return 0;
}

int main(int argc, char** argv)
{
    if (argc == 3 && std::string(argv[1]) == "--parse")
        return print_parsed(argv[2]);

    // ---- is a master set at all ------------------------------------------
    expect(Master_NameIsSet(nullptr), false, "null name is unset");
    expect(Master_NameIsSet(""), false, "empty name is unset");
    expect(Master_NameIsSet(" \t\r\n"), false, "blank name is unset");
    expect(Master_NameIsSet("master.example"), true, "a host name is set");
    expect(Master_NameIsSet(" 192.0.2.1"), true, "a name after a space is set");
    expect(Master_NameHasPort("master.example"), false, "bare host has no port");
    expect(Master_NameHasPort("master.example:27950"), true, "host:port has a port");
    expect(Master_NameHasPort(nullptr), false, "null name has no port");

    // ---- which addresses a master may list ----------------------------------
    const uint8_t ordinary[4] = {192, 0, 2, 1};
    const uint8_t loopback[4] = {127, 0, 0, 1};
    const uint8_t lan[4] = {10, 0, 0, 7};
    const uint8_t zero[4] = {0, 0, 0, 0};
    const uint8_t zeroNet[4] = {0, 1, 2, 3};
    const uint8_t multicast[4] = {224, 0, 0, 1};
    const uint8_t reserved[4] = {240, 0, 0, 1};
    const uint8_t broadcast[4] = {255, 255, 255, 255};
    const uint8_t top[4] = {223, 255, 255, 255};
    expect(Master_AddressIsListable(ordinary, 0x71, 0x20), true, "an ordinary address is listable");
    expect(Master_AddressIsListable(loopback, 0x71, 0x20), true, "loopback is listable (master on the same machine)");
    expect(Master_AddressIsListable(lan, 0x71, 0x20), true, "a private address is listable (master on the LAN)");
    expect(Master_AddressIsListable(top, 0, 1), true, "223.255.255.255 port 1 is listable");
    expect(Master_AddressIsListable(ordinary, 0, 0), false, "port 0 is not listable");
    expect(Master_AddressIsListable(zero, 0x71, 0x20), false, "0.0.0.0 is not listable");
    expect(Master_AddressIsListable(zeroNet, 0x71, 0x20), false, "0.x.x.x is not listable");
    expect(Master_AddressIsListable(multicast, 0x71, 0x20), false, "multicast is not listable");
    expect(Master_AddressIsListable(reserved, 0x71, 0x20), false, "240.0.0.0/4 is not listable");
    expect(Master_AddressIsListable(broadcast, 0x71, 0x20), false, "broadcast is not listable");

    // ---- getserversResponse --------------------------------------------------
    std::vector<MasterServerEntry> out;
    int skipped = -1;

    expect_int(parse(header + entry(192, 0, 2, 1, 28960) + entry(198, 51, 100, 2, 28961) + eot, &out, 256, &skipped), 2,
        "two entries and EOT");
    expect(out.size() == 2 && is(out[0], 192, 0, 2, 1, 28960) && is(out[1], 198, 51, 100, 2, 28961), true,
        "entries keep their order, address and port");
    expect_int(skipped, 0, "nothing skipped in a clean list");

    expect_int(parse(header + eot, &out, 256, &skipped), 0, "an empty list");
    expect_int(parse(header + "\\EOT", &out, 256, &skipped), 0, "an empty list with a short EOT");
    expect_int(parse(header, &out, 256, &skipped), 0, "no entries at all");
    expect_int(parse(header + "\\", &out, 256, &skipped), 0, "a lone separator");
    expect_int(parse("", &out, 256, &skipped), 0, "an empty datagram");
    expect_int(parse(std::string(64, 'A'), &out, 256, &skipped), 0, "no separator anywhere");

    // Retail stopped at any entry starting "EOT", so 69.79.84.x could never be listed.
    expect_int(parse(header + entry(69, 79, 84, 9, 28960) + entry(192, 0, 2, 1, 28960) + eot, &out, 256, &skipped), 2,
        "an address whose bytes spell EOT is an address");
    expect(out.size() == 2 && is(out[0], 69, 79, 84, 9, 28960), true, "69.79.84.9 is listed");

    // Address and port bytes may be backslashes.
    expect_int(parse(header + entry(92, 92, 92, 92, 0x5c5c) + eot, &out, 256, &skipped), 1, "an entry made of backslashes");
    expect(out.size() == 1 && is(out[0], 92, 92, 92, 92, 0x5c5c), true, "92.92.92.92:23644 is read whole");

    // An entry counts only when a separator follows it, as in retail.
    expect_int(parse(header + entry(192, 0, 2, 1, 28960), &out, 256, &skipped), 0, "a last entry with no separator is dropped");
    expect_int(parse(header + entry(192, 0, 2, 1, 28960) + "\\", &out, 256, &skipped), 1, "a separator after it makes it count");
    expect_int(parse(header + entry(192, 0, 2, 1, 28960) + "X" + entry(192, 0, 2, 2, 28960) + eot, &out, 256, &skipped), 0,
        "garbage after an entry stops the list before it");
    for (size_t cut = 0; cut < 7; ++cut)
    {
        const std::string truncated = header + entry(192, 0, 2, 1, 28960) + entry(192, 0, 2, 2, 28960).substr(0, cut);
        expect_int(parse(truncated, &out, 256, &skipped), cut == 0 ? 0 : 1, "an entry cut short is not read");
    }

    // Unlistable addresses are skipped, not fatal.
    expect_int(parse(header + entry(0, 0, 0, 0, 28960) + entry(192, 0, 2, 1, 28960) + entry(255, 255, 255, 255, 28960)
        + entry(192, 0, 2, 2, 0) + entry(224, 0, 0, 1, 28960) + entry(192, 0, 2, 3, 1) + eot, &out, 256, &skipped), 2,
        "unlistable addresses are skipped");
    expect_int(skipped, 4, "four unlistable addresses counted");
    expect(out.size() == 2 && is(out[0], 192, 0, 2, 1, 28960) && is(out[1], 192, 0, 2, 3, 1), true,
        "the listable ones around them are kept");

    // Limits.
    std::string big = header;
    for (int i = 0; i < 400; ++i)
        big += entry(10, 1, i >> 8, i & 0xff, 28960);
    big += eot;
    expect_int(parse(big, &out, 1000, &skipped), MASTER_MAX_ENTRIES_PER_PACKET, "at most 256 entries per packet");
    expect_int(parse(big, &out, 10, &skipped), 10, "never more than the caller has room for");
    expect(out.size() == 10 && is(out[9], 10, 1, 0, 9, 28960), true, "the first ten are the ones kept");
    MasterServerEntry one{};
    expect_int(Master_ParseServersResponse(nullptr, 100, &one, 1), 0, "null data");
    expect_int(Master_ParseServersResponse(reinterpret_cast<const uint8_t*>("\\"), -5, &one, 1), 0, "negative size");
    expect_int(Master_ParseServersResponse(reinterpret_cast<const uint8_t*>("\\"), 1, nullptr, 1), 0, "null output");
    expect_int(Master_ParseServersResponse(reinterpret_cast<const uint8_t*>("\\"), 1, &one, 0), 0, "no room");

    // ---- who may answer -------------------------------------------------------
    MasterQuery query{};
    const uint8_t masterIp[4] = {192, 0, 2, 53};
    const uint8_t otherIp[4] = {192, 0, 2, 54};
    expect(Master_QueryAccepts(&query, masterIp, 1234, 0), false, "no list is accepted before any request");
    expect(Master_QueryAccepts(nullptr, masterIp, 1234, 0), false, "null query accepts nothing");
    Master_QueryAsked(&query, masterIp, 1234, 5000);
    expect(Master_QueryAccepts(&query, masterIp, 1234, 5000), true, "the asked master may answer at once");
    expect(Master_QueryAccepts(&query, masterIp, 1234, 5000 + MASTER_RESPONSE_WINDOW_MS), true,
        "the asked master may answer until the window closes");
    expect(Master_QueryAccepts(&query, masterIp, 1234, 5001 + MASTER_RESPONSE_WINDOW_MS), false,
        "a list after the window is refused");
    expect(Master_QueryAccepts(&query, otherIp, 1234, 5000), false, "another address is refused");
    expect(Master_QueryAccepts(&query, masterIp, 1235, 5000), false, "another port is refused");
    expect(Master_QueryAccepts(&query, masterIp, 1234, 4999), false, "a clock before the request is refused");
    Master_QueryAsked(&query, masterIp, 1234, 0xFFFFFF00u);
    expect(Master_QueryAccepts(&query, masterIp, 1234, 0x00000100u), true, "the window survives the millisecond wrap");
    Master_QueryAsked(&query, otherIp, 99, 7000);
    expect(Master_QueryAccepts(&query, masterIp, 1234, 7000), false, "a new request replaces the old master");
    expect(Master_QueryAccepts(&query, otherIp, 99, 7000), true, "the newly asked master may answer");

    // ---- server side -----------------------------------------------------------
    expect(Master_ServerUsesMaster(2, "master.example"), true, "dedicated 2 with a master reports to it");
    expect(Master_ServerUsesMaster(2, ""), false, "dedicated 2 without a master reports to nobody");
    expect(Master_ServerUsesMaster(2, nullptr), false, "dedicated 2 with a null master reports to nobody");
    expect(Master_ServerUsesMaster(1, "master.example"), false, "a LAN dedicated server never reports");
    expect(Master_ServerUsesMaster(0, "master.example"), false, "a listen server never reports");
    expect(Master_HeartbeatDue(2, "master.example", 1000, INT_MIN), true, "a forced heartbeat is due");
    expect(Master_HeartbeatDue(2, "master.example", 1000, 1000), true, "a heartbeat is due at its time");
    expect(Master_HeartbeatDue(2, "master.example", 999, 1000), false, "a heartbeat is not due early");
    expect(Master_HeartbeatDue(2, "", 1000, INT_MIN), false, "no heartbeat without a master, even when forced");
    expect(Master_HeartbeatDue(1, "master.example", 1000, INT_MIN), false, "no heartbeat from a LAN server");
    expect_int(MASTER_HEARTBEAT_MSEC, 180000, "retail's three-minute heartbeat");

    MasterResolveState state{};
    expect(Master_ShouldResolve(&state, "", 20810, 0), false, "nothing to look up without a master");
    expect(Master_ShouldResolve(&state, "master.example", 20810, 0), true, "a new name is looked up");
    Master_ResolveAttempted(&state, "master.example", 20810, true, 1000);
    expect(Master_ShouldResolve(&state, "master.example", 20810, 5000000), false, "a resolved name is not looked up again");
    expect(Master_ShouldResolve(&state, "other.example", 20810, 1001), true, "a changed name is looked up at once");
    expect(Master_ShouldResolve(&state, "master.example", 20811, 1001), true, "a changed port is looked up at once");
    Master_ResolveAttempted(&state, "master.example", 20810, false, 1000);
    expect(Master_ShouldResolve(&state, "master.example", 20810, 1000 + MASTER_RESOLVE_RETRY_MS - 1), false,
        "a failed lookup is not retried early");
    expect(Master_ShouldResolve(&state, "master.example", 20810, 1000 + MASTER_RESOLVE_RETRY_MS), true,
        "a failed lookup is retried after 15 minutes");
    const std::string longName(400, 'm');
    Master_ResolveAttempted(&state, longName.c_str(), 20810, true, 0);
    expect(Master_ShouldResolve(&state, longName.c_str(), 20810, 1), false, "a very long name is remembered without overflow");
    expect(Master_ShouldResolve(nullptr, "master.example", 20810, 0), false, "null state");

    if (failures)
        std::printf("%d rule check(s) failed\n", failures);
    return failures ? 1 : 0;
}
'''


def toolchain_supports(compiler: str, flags: list, workdir: Path) -> bool:
    probe = workdir / "probe.cpp"
    probe.write_text("int main() { return 0; }\n")
    result = subprocess.run(
        [compiler, *flags, "-std=c++20", str(probe), "-o", str(workdir / "probe")],
        stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
    )
    return result.returncode == 0


def check_registration() -> bool:
    """The production registration of masterServerName has the empty default."""
    ok = True
    text = COMMON.read_text(encoding="utf-8", errors="replace")
    match = re.search(r'Dvar_RegisterString\(\s*"masterServerName",\s*("(?:[^"\\]|\\.)*")\s*,\s*(\w+)', text)
    if not match:
        print("FAIL: common.cpp no longer registers masterServerName the way this check reads it")
        return False
    if match.group(1) != '""':
        print(f"FAIL: masterServerName defaults to {match.group(1)}; it must be empty so nobody is contacted by default")
        ok = False
    if match.group(2) != "DVAR_ARCHIVE":
        print(f"FAIL: masterServerName is registered with {match.group(2)}, expected DVAR_ARCHIVE")
        ok = False
    if ok:
        print("PASS: masterServerName is registered with an empty default")
    return ok


def oob(text: bytes) -> bytes:
    return b"\xff\xff\xff\xff" + text


def check_local_master(parser_binary: Path) -> bool:
    """Drives scripts/tools/local_master_server.py over loopback and reads its list with the production parser."""
    failures = []

    def expect(condition, what):
        if not condition:
            failures.append(what)
            print(f"FAIL: local test master: {what}")

    def udp():
        sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        sock.bind(("127.0.0.1", 0))
        sock.settimeout(3.0)
        return sock

    def receive(sock):
        try:
            return sock.recvfrom(4096)[0]
        except socket.timeout:
            return None

    def quiet(sock):
        sock.settimeout(0.2)
        try:
            sock.recvfrom(4096)
            return False
        except socket.timeout:
            return True
        finally:
            sock.settimeout(3.0)

    master_sock = udp()
    master_sock.settimeout(None)
    master_address = master_sock.getsockname()
    stop = threading.Event()
    master = local_master_server.Master(master_sock)
    thread = threading.Thread(target=master.serve, args=(stop.is_set,), daemon=True)
    thread.start()

    def register(sock, info):
        sock.sendto(oob(b"heartbeat COD-4\n"), master_address)
        question = receive(sock)
        if not question or not question.startswith(oob(b"getinfo ")):
            return False
        challenge = question[len(oob(b"getinfo ")):]
        sock.sendto(oob(b"infoResponse\n\\challenge\\" + challenge + info), master_address)
        return True

    def ask(sock, request):
        sock.sendto(oob(request), master_address)
        reply = receive(sock)
        if reply is None:
            return None
        parsed = subprocess.run([str(parser_binary), "--parse", reply.hex()], capture_output=True, text=True, check=True)
        return reply, sorted(parsed.stdout.split())

    try:
        client = udp()
        empty_server, busy_server, old_server, liar, keyless = udp(), udp(), udp(), udp(), udp()

        reply = ask(client, b"getservers 1 full empty")
        expect(reply is not None and reply[0] == oob(b"getserversResponse\\EOT\0\0\0") and reply[1] == [],
               "an empty master answers with an empty list")

        # The same keys a KisakCOD server that reports to a master sends.
        expect(register(empty_server, b"\\protocol\\1\\hostname\\Empty\\mapname\\mp_crash\\clients\\0"
                                      b"\\sv_maxclients\\24\\gametype\\war\\pure\\1\\hw\\2\\mod\\0\\voice\\0\\pb\\0"),
               "a heartbeat is answered with a getinfo challenge")
        expect(register(busy_server, b"\\protocol\\1\\hostname\\Busy\\clients\\24\\sv_maxclients\\24\\gametype\\sd"),
               "a second server is challenged")
        expect(register(old_server, b"\\protocol\\6\\hostname\\Retail\\clients\\3\\sv_maxclients\\24\\gametype\\war"),
               "a protocol 6 server is challenged")
        # Retail's reply for an empty server has no "clients" key; dpmaster's rule refuses it.
        expect(register(keyless, b"\\protocol\\1\\hostname\\NoClientsKey\\sv_maxclients\\24\\gametype\\war"),
               "a server without the clients key is challenged")

        # A wrong challenge must not register.
        liar.sendto(oob(b"heartbeat COD-4\n"), master_address)
        expect(receive(liar) is not None, "the liar is challenged too")
        liar.sendto(oob(b"infoResponse\n\\challenge\\wrong\\protocol\\1\\clients\\1\\sv_maxclients\\24"), master_address)
        # And an infoResponse nobody asked for must not either.
        stranger = udp()
        stranger.sendto(oob(b"infoResponse\n\\challenge\\x\\protocol\\1\\clients\\1\\sv_maxclients\\24"), master_address)

        def name(sock):
            return f"127.0.0.1:{sock.getsockname()[1]}"

        reply = ask(client, b"getservers 1 full empty")
        expect(reply is not None and reply[1] == sorted([name(empty_server), name(busy_server)]),
               "protocol 1, full and empty wanted: both protocol 1 servers and nobody else")
        reply = ask(client, b"getservers 1")
        expect(reply is not None and reply[1] == [], "protocol 1 without keywords: empty and full servers are left out")
        reply = ask(client, b"getservers 1 empty")
        expect(reply is not None and reply[1] == [name(empty_server)], "protocol 1, empty wanted: the empty server only")
        reply = ask(client, b"getservers 6 full empty")
        expect(reply is not None and reply[1] == [name(old_server)], "protocol 6: the protocol 6 server only")
        reply = ask(client, b"getservers 2 full empty")
        expect(reply is not None and reply[1] == [], "an unused protocol: nobody")

        client.sendto(oob(b"getservers"), master_address)
        client.sendto(oob(b"getservers abc"), master_address)
        client.sendto(b"getservers 1 full empty", master_address)
        client.sendto(oob(b"rcon secret status"), master_address)
        expect(quiet(client), "malformed and unrelated requests get no answer")

        busy_server.sendto(oob(b"heartbeat flatline\n"), master_address)
        expect(quiet(busy_server), "a flatline heartbeat is not challenged")
        reply = ask(client, b"getservers 1 full empty")
        expect(reply is not None and reply[1] == [name(empty_server)], "a server that sent flatline is no longer listed")

        # Many servers: several packets, each a complete list the parser accepts.
        crowd = [udp() for _ in range(local_master_server.MAX_PACKET_ENTRIES + 20)]
        for sock in crowd:
            register(sock, b"\\protocol\\3\\clients\\1\\sv_maxclients\\8")
        client.sendto(oob(b"getservers 3"), master_address)
        seen = []
        while True:
            packet = receive(client) if not seen or len(seen) < len(crowd) else None
            if packet is None:
                break
            parsed = subprocess.run([str(parser_binary), "--parse", packet.hex()], capture_output=True, text=True, check=True)
            expect(packet.endswith(b"\\EOT\0\0\0") and len(packet) < 1400, "each list packet is complete and fits a datagram")
            seen.extend(parsed.stdout.split())
        expect(sorted(seen) == sorted(name(sock) for sock in crowd), "a long list arrives whole across several packets")
    finally:
        stop.set()
        thread.join(timeout=3)
        master_sock.close()

    if not failures:
        print("PASS: local test master speaks the protocol and the production parser reads its lists")
    return not failures


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--cxx", default=os.environ.get("CXX", "clang++"))
    parser.add_argument("--m32", action="store_true",
                        help="require a 32-bit build in addition to the 64-bit one")
    parser.add_argument("--no-sanitize", action="store_true",
                        help="compile without -fsanitize=address,undefined")
    args = parser.parse_args()

    if not HEADER.is_file():
        print(f"FAIL: {HEADER} is missing", file=sys.stderr)
        return 1

    ok = check_registration()

    with tempfile.TemporaryDirectory(prefix="kisak-master-server-") as temp:
        workdir = Path(temp)
        source = workdir / "master_server_check.cpp"
        source.write_text(PROGRAM)

        sanitize = [] if args.no_sanitize else ["-fsanitize=address,undefined", "-fno-sanitize-recover=all"]
        if sanitize and not toolchain_supports(args.cxx, sanitize, workdir):
            print("NOTE: address/undefined sanitizers are unavailable for this toolchain; building without them")
            sanitize = []

        widths = [("64", list(sanitize))]
        if toolchain_supports(args.cxx, sanitize + ["-m32"], workdir):
            widths.append(("32", sanitize + ["-m32"]))
        elif toolchain_supports(args.cxx, ["-m32"], workdir):
            print("NOTE: no 32-bit sanitizer runtime; the 32-bit configuration is built without sanitizers")
            widths.append(("32", ["-m32"]))
        elif args.m32:
            print("FAIL: --m32 requires a 32-bit build and this toolchain cannot produce one", file=sys.stderr)
            return 1
        else:
            print("NOTE: this toolchain cannot build 32-bit; only the 64-bit configuration runs")

        for width, flags in widths:
            executable = workdir / f"master_server_check{width}"
            command = [args.cxx, "-std=c++20", "-Wall", "-Wextra", "-Werror", *flags,
                       f"-I{HEADER.parent}", str(source), "-o", str(executable)]
            print("+", " ".join(shlex.quote(part) for part in command))
            subprocess.run(command, check=True)
            print("+", shlex.quote(str(executable)))
            result = subprocess.run([str(executable)])
            if result.returncode != 0:
                print(f"FAIL: {width}-bit master-server rule checks failed", file=sys.stderr)
                ok = False
                continue
            print(f"PASS: {width}-bit master-server rule checks")
            if not check_local_master(executable):
                ok = False
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
