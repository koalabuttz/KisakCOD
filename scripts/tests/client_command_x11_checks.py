"""Real console-command/UDP checks used by test_linux_window_x11 --commands.

All commands are typed into the local diagnostic's console. UDP peers are bound
to loopback and only answer read-only discovery; packet text must never become
a console command. No authentication or game connection is attempted.
"""

import errno
import re
import socket
import uuid


class CommandChecks:
    def __init__(self, xdo, wait_for, read, trace, process, timeout):
        self.xdo, self.wait_for, self.read = xdo, wait_for, read
        self.trace, self.process, self.timeout = trace, process, timeout
        self.tag = uuid.uuid4().hex[:12]
        self.local_port = None
        self.never = "NEVER_EXECUTE_" + self.tag

    def log(self):
        return self.read(self.trace)

    def command(self, text):
        start = len(self.log())
        self.xdo("key", "ctrl+c")
        self.wait_for(lambda: "editor text= cursor=0 held=0\n" in self.log()[start:],
                      self.timeout, "console cleared for local command", self.process)
        start = len(self.log())
        self.xdo("type", "--clearmodifiers", "--delay", "0", "--", text)
        self.wait_for(lambda: f"editor text={text} cursor={len(text)} held=0\n" in self.log()[start:],
                      self.timeout, "local command typed through real editor", self.process)
        self.xdo("key", "Return")
        return start

    def marker(self, marker, start):
        return self.wait_for(lambda: marker in self.log()[start:], self.timeout, marker, self.process)

    @staticmethod
    def echo_lines(log):
        return re.findall(r"^echo (.*)$", log, re.M)

    @staticmethod
    def peer():
        peer = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        peer.bind(("127.0.0.1", 0))
        peer.settimeout(2)
        return peer

    def query(self, peer):
        start = self.command(f"discover 127.0.0.1:{peer.getsockname()[1]}")
        packet, source = peer.recvfrom(2048)
        if not re.fullmatch(b"\xff{4}getinfo [0-9a-f]{16}", packet):
            raise RuntimeError(f"unexpected discovery datagram: {packet!r}")
        return start, packet[12:], source

    @staticmethod
    def response(token, extra=b""):
        return b"\xff" * 4 + b"infoResponse\n\\challenge\\" + token + extra

    @staticmethod
    def require_bound(port):
        with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as probe:
            try:
                probe.bind(("127.0.0.1", port))
            except OSError as error:
                if error.errno == errno.EADDRINUSE:
                    return
                raise
        raise RuntimeError(f"expected live client socket on port {port}")

    def run(self):
        before, after = "BEFORE_" + self.tag, "AFTER_" + self.tag
        start = self.command(f"echo {before}; wait 2; echo {after}")
        self.marker("echo " + after + "\n", start)
        tick, ticks = None, {}
        for line in self.log()[start:].splitlines():
            match = re.fullmatch(r"command_tick=(\d+)", line)
            if match:
                tick = int(match.group(1))
            if line in ("echo " + before, "echo " + after):
                ticks.setdefault(line[5:], []).append(tick)
        if (set(ticks) != {before, after} or any(len(values) != 1 or values[0] is None for values in ticks.values())
                or ticks[after][0] < ticks[before][0] + 2):
            raise RuntimeError(f"queued commands did not execute once across wait ticks: {ticks}")
        print("PASS: real Cbuf echo/wait/echo executes once across separate ticks", flush=True)

        with self.peer() as peer, self.peer() as impostor:
            unknown = "unknown_" + self.tag
            done = "INVALID_DONE_" + self.tag
            start = self.command(f"{unknown}; discover; discover 127.0.0.1:0; echo {done}")
            self.marker("echo " + done + "\n", start)
            for expected in ("Unknown command (not forwarded): " + unknown,
                             "discover requires explicit IPv4:port", "discovery=invalid"):
                self.marker(expected, start)
            if "discovery=sent" in self.log()[start:]:
                raise RuntimeError("invalid discovery command sent a packet")
            print("PASS: unknown and invalid commands report errors and leave executor responsive", flush=True)

            start, token, source = self.query(peer)
            self.require_bound(source[1])
            # Same endpoint but wrong token and non-discovery command text fail.
            peer.sendto(self.response(b"wrong"), source)
            peer.sendto(b"\xff" * 4 + b"echo " + self.never.encode(), source)
            # Correct token from a different endpoint must not complete discovery.
            impostor.sendto(self.response(token), source)
            barrier = "SOURCE_CHECK_" + self.tag
            self.marker("echo " + barrier + "\n", self.command("echo " + barrier))
            if "discovery=received" in self.log()[start:]:
                raise RuntimeError("wrong challenge/source completed discovery")
            peer.sendto(self.response(token, b"\\hostname\\echo " + self.never.encode()), source)
            self.marker("discovery=received discarded=2 connected=false", start)
            if self.never in self.echo_lines(self.log()):
                raise RuntimeError("remote packet text executed as a local command")
            print("PASS: real UDP discovery validates endpoint/challenge; packet text is not executed", flush=True)

            # Restart while the process remains alive, then bind the retired
            # port ourselves. This observes real close rather than exit cleanup.
            self.marker("network=restarted connected=false", self.command("net_restart"))
            with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as retired:
                retired.bind(("127.0.0.1", source[1]))
                pending_start, pending_token, pending_source = self.query(peer)
                if pending_source[1] == source[1]:
                    raise RuntimeError("restarted client reused the test-reserved retired socket")
                # Cancel an outstanding session by restarting, then recreate it.
                cancel = self.command("net_restart")
                self.marker("network=restarted connected=false", cancel)
                with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as cancelled:
                    cancelled.bind(("127.0.0.1", pending_source[1]))
                    start, fresh_token, fresh_source = self.query(peer)
                    if fresh_token == pending_token:
                        raise RuntimeError("recreated discovery reused its old challenge")
                    peer.sendto(self.response(pending_token), fresh_source)
                    peer.sendto(self.response(fresh_token), fresh_source)
                    self.marker("discovery=received discarded=1 connected=false", start)
                    self.local_port = fresh_source[1]
                    self.require_bound(self.local_port)
            peer.settimeout(0.15)
            try:
                extra, _ = peer.recvfrom(2048)
            except socket.timeout:
                pass
            else:
                raise RuntimeError(f"unexpected post-discovery packet (no admission allowed): {extra!r}")
            print("PASS: live network restart releases ports, cancels pending discovery, and accepts a fresh session", flush=True)
        self.marker("status tick=", self.command("status"))

    def prepare_shutdown(self):
        # The remaining command must be dropped by shutdown, not executed as
        # an accidental final pump. It is never transmitted anywhere.
        self.command(f"wait 100; echo {self.never}")

    def verify_shutdown(self):
        if self.never in self.echo_lines(self.log()):
            raise RuntimeError("a remote or pending-after-shutdown command executed")
        if self.local_port is not None:
            with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as released:
                released.bind(("127.0.0.1", self.local_port))
        print("PASS: pending command stays unexecuted on close and final client socket is released", flush=True)
