#!/usr/bin/env python3
"""Opt-in native dedicated-server lifecycle soak with installed, licensed assets.

Binds IPv4 loopback only; never connects a player or bypasses authentication.
Exercises console lifecycle commands, challenge-correlated UDP queries, resource
samples, and console/SIGTERM shutdown. Assets are symlinked, not copied; writable
files live in a disposable home. Reports contain logs/measurements, not assets.
"""
import argparse
import json
import math
import os
from pathlib import Path
import re
import secrets
import select
import signal
import socket
import subprocess
import tempfile
import time

ROOT = Path(__file__).resolve().parents[2]
OOB = b"\xff" * 4


class CheckFailure(RuntimeError):
    pass


def parse_reply(packet, kind, challenge):
    """Reject stale, wrong-kind and malformed replies; return the info dictionary."""
    prefix = OOB + kind.encode("ascii") + b"\n"
    if not packet.startswith(prefix):
        return None
    line = packet[len(prefix):].split(b"\n", 1)[0].rstrip(b"\x00\r")
    try:
        parts = line.decode("utf-8", "strict").split("\\")
    except UnicodeDecodeError:
        return None
    if not parts or parts[0] or len(parts) % 2 != 1:
        return None
    values = {}
    for key, value in zip(parts[1::2], parts[2::2]):
        if not key or key in values:
            return None
        values[key] = value
    return values if values.get("challenge") == challenge else None


def sample_process(pid):
    proc = Path("/proc") / str(pid)
    stat = (proc / "stat").read_text()
    fields = stat[stat.rfind(")") + 2:].split()
    memory = [int(value) for value in (proc / "statm").read_text().split()]
    status = (proc / "status").read_text()
    threads = re.search(r"^Threads:\s+(\d+)$", status, re.MULTILINE)
    return {"monotonic": time.monotonic(), "rss_bytes": memory[1] * os.sysconf("SC_PAGE_SIZE"),
            "virtual_bytes": memory[0] * os.sysconf("SC_PAGE_SIZE"),
            "cpu_seconds": (int(fields[11]) + int(fields[12])) / os.sysconf("SC_CLK_TCK"),
            "threads": int(threads.group(1)), "fds": len(list((proc / "fd").iterdir()))}


def resource_growth(baseline, later):
    return {"rss_bytes": later["rss_bytes"] - baseline["rss_bytes"],
            "fds": later["fds"] - baseline["fds"],
            "threads": later["threads"] - baseline["threads"]}


class Server:
    def __init__(self, binary, game, work, output, timeout):
        self.timeout, self.output = timeout, output
        self.samples, self.stages, self.cycle_endpoints = [], [], []
        self.next_sample = 0.0
        self.log_path = output / "server.log"
        assets, home = work / "assets", work / "home"
        assets.mkdir()
        home.mkdir()
        for name in ("main", "zone"):
            (assets / name).symlink_to(game / name, target_is_directory=True)
        (home / "localization.txt").symlink_to(game / "localization.txt")
        with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as reservation:
            reservation.bind(("127.0.0.1", 0))
            port = reservation.getsockname()[1]
        self.address = ("127.0.0.1", port)
        self.hostname = "kisak-test-" + secrets.token_hex(6)
        command = [str(binary), "+set", "fs_basepath", str(assets), "+set", "fs_homepath", str(home),
                   "+set", "net_ip", "127.0.0.1", "+set", "net_port", str(port),
                   "+set", "dedicated", "1", "+set", "sv_hostname", self.hostname,
                   "+map", "mp_crash"]
        self.log_stream = self.log_path.open("wb")
        self.process = None
        self.socket = None
        try:
            self.process = subprocess.Popen(command, cwd=home, stdin=subprocess.PIPE,
                                            stdout=self.log_stream, stderr=subprocess.STDOUT,
                                            start_new_session=True)
            self.socket = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
            self.socket.bind(("127.0.0.1", 0))
            self.socket.settimeout(0.25)
        except BaseException:
            self.cleanup()
            raise
        self.command = command

    def log(self):
        return self.log_path.read_text(errors="replace")

    def alive(self):
        code = self.process.poll()
        if code is not None:
            raise CheckFailure(f"server exited unexpectedly: {code}; see {self.log_path}")
        text = self.log()
        if "ASSERT FAILED" in text or "FATAL ERROR" in text:
            raise CheckFailure(f"assertion/fatal error; see {self.log_path}")

    def sample(self, force=False):
        now = time.monotonic()
        if force or now >= self.next_sample:
            try:
                self.samples.append(sample_process(self.process.pid))
            except (OSError, ValueError, AttributeError) as error:
                raise CheckFailure(f"cannot sample live server: {error}") from error
            self.next_sample = now + 1
        return self.samples[-1] if self.samples else None

    def send(self, command):
        self.alive()
        self.process.stdin.write((command + "\n").encode("ascii"))
        self.process.stdin.flush()

    def query(self, command, expected_map, timeout=None):
        challenge = secrets.token_hex(12)
        expected_kind = {"getinfo": "infoResponse", "getstatus": "statusResponse"}[command]
        request = OOB + f"{command} {challenge}\n".encode("ascii")
        deadline = time.monotonic() + (self.timeout if timeout is None else timeout)
        next_send = 0.0
        while time.monotonic() < deadline:
            self.alive()
            self.sample()
            now = time.monotonic()
            if now >= next_send:
                self.socket.sendto(request, self.address)
                next_send = now + 0.5
            try:
                packet, source = self.socket.recvfrom(65535)
            except socket.timeout:
                continue
            if source != self.address:
                continue
            values = parse_reply(packet, expected_kind, challenge)
            if values is None or values.get("mapname") != expected_map:
                continue
            hostname = values.get("hostname") if command == "getinfo" else values.get("sv_hostname")
            if hostname != self.hostname:
                continue  # Never accept another process after a bind-port race.
            if values.get("protocol") != "1":
                raise CheckFailure(f"unexpected query protocol: {values.get('protocol')!r}")
            return values
        raise CheckFailure(f"timed out waiting for fresh {command} reply on {expected_map}")

    def hold(self, map_name, duration):
        deadline = time.monotonic() + duration
        while time.monotonic() < deadline:
            self.query("getinfo", map_name, timeout=min(5, self.timeout))
            self.query("getstatus", map_name, timeout=min(5, self.timeout))
            # A blocking select paces this measurement interval without a CPU spin.
            select.select([], [], [], min(1.0, max(0, deadline - time.monotonic())))
        self.sample(force=True)

    def transition(self, label, command, map_name, hold_seconds):
        offset = self.log_path.stat().st_size
        if command:
            self.send(command)
        self.send("status")
        self.query("getinfo", map_name)
        deadline = time.monotonic() + self.timeout
        while time.monotonic() < deadline:
            self.query("getstatus", map_name, timeout=min(5, self.timeout))
            segment = self.log_path.read_bytes()[offset:].decode(errors="replace")
            if f"map: {map_name}\n" in segment:
                if command == "map_restart" and f"Server: {map_name}\n" not in segment:
                    continue
                break
        else:
            raise CheckFailure(f"no console lifecycle/status completion for {label}")
        self.hold(map_name, hold_seconds)
        snapshot = self.sample(force=True)
        self.stages.append({"label": label, "command": command, "map": map_name,
                            "sample": snapshot})
        print(f"PASS: {label}: {map_name}, fresh getinfo/getstatus, console status", flush=True)
        return snapshot

    def shutdown(self, method):
        offset = self.log_path.stat().st_size
        if method == "console":
            self.send("quit")
        else:
            self.alive()
            self.process.send_signal(signal.SIGTERM)
        try:
            code = self.process.wait(timeout=self.timeout)
        except subprocess.TimeoutExpired as error:
            raise CheckFailure(f"{method} shutdown timed out") from error
        text = self.log_path.read_bytes()[offset:].decode(errors="replace")
        quit_at, cleanup_at = text.find("quitting..."), text.find("ShutdownGame")
        if code != 0 or quit_at < 0 or cleanup_at <= quit_at:
            raise CheckFailure(f"{method} shutdown failed: exit {code}, missing fresh ordered game cleanup")
        if "ASSERT FAILED" in text or "FATAL ERROR" in text:
            raise CheckFailure(f"error during {method} shutdown")
        print(f"PASS: {method} shutdown: exit 0 and game cleanup", flush=True)

    def cleanup(self):
        if self.process is not None:
            if self.process.poll() is None:
                self.process.kill()
            self.process.wait()
            if self.process.stdin is not None:
                self.process.stdin.close()
        if self.socket is not None:
            self.socket.close()
        self.log_stream.close()


def run_session(args, output, method, cycles):
    report = {"shutdown": method, "stages": [], "samples": [], "cycle_endpoints": []}
    server = None
    failure = "session did not complete"
    temporary = tempfile.TemporaryDirectory(prefix="kisak-lifecycle-")
    try:
        server = Server(args.binary, args.game_dir, Path(temporary.name), output, args.timeout)
        server.transition("initial map", None, "mp_crash", args.hold_seconds)
        for cycle in range(cycles):
            server.transition(f"cycle {cycle + 1} restart", "map_restart", "mp_crash", args.hold_seconds)
            server.transition(f"cycle {cycle + 1} map change", "map mp_shipment", "mp_shipment", args.hold_seconds)
            server.send('set sv_mapRotation "map mp_crash map mp_shipment"')
            server.send('set sv_mapRotationCurrent ""')
            endpoint = server.transition(f"cycle {cycle + 1} rotation", "map_rotate", "mp_crash", args.hold_seconds)
            server.cycle_endpoints.append(endpoint)
        # Ignore the first full cycle's legitimate lazy initialization.
        if len(server.cycle_endpoints) >= 2:
            baseline = server.cycle_endpoints[0]
            growth = [resource_growth(baseline, point) for point in server.cycle_endpoints[1:]]
            report["growth_after_warmup"] = growth
            if any(g["rss_bytes"] > args.max_rss_growth_mib * 1024 * 1024 or
                   g["fds"] > args.max_fd_growth or g["threads"] > 0 for g in growth):
                raise CheckFailure(f"resource growth exceeded thresholds: {growth}")
            print(f"PASS: same-map resources after warm-up: {growth}", flush=True)
        server.shutdown(method)
        report.update(command=server.command, stages=server.stages, samples=server.samples,
                      cycle_endpoints=server.cycle_endpoints, exit_code=server.process.returncode)
        failure = None
    except (CheckFailure, OSError, subprocess.SubprocessError) as error:
        failure = str(error)
        print(f"FAIL: {failure}", flush=True)
    except BaseException as error:
        failure = f"{type(error).__name__}: {error}"
        raise
    finally:
        try:
            if server:
                report.update(stages=server.stages, samples=server.samples,
                              cycle_endpoints=server.cycle_endpoints)
                server.cleanup()
        finally:
            temporary.cleanup()  # Never remove the writable home before the child exits.
            report["passed"] = failure is None
            report["error"] = failure
            (output / "report.json").write_text(json.dumps(report, indent=2) + "\n")
    return failure is None


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("game_dir", type=Path)
    parser.add_argument("--binary", type=Path, default=ROOT / "bin/KisakCOD-dedi")
    parser.add_argument("--output-dir", type=Path, required=True, help="new directory for text logs and JSON")
    parser.add_argument("--cycles", type=int, default=3)
    parser.add_argument("--hold-seconds", type=float, default=15)
    parser.add_argument("--timeout", type=float, default=60)
    parser.add_argument("--shutdown", choices=("console", "sigterm", "both"), default="both")
    parser.add_argument("--max-rss-growth-mib", type=float, default=64)
    parser.add_argument("--max-fd-growth", type=int, default=2)
    args = parser.parse_args()
    args.game_dir, args.binary = args.game_dir.expanduser().resolve(), args.binary.expanduser().resolve()
    if args.cycles < 1 or not math.isfinite(args.hold_seconds) or args.hold_seconds < 0 or not math.isfinite(args.timeout) or args.timeout <= 0:
        parser.error("cycles must be positive; hold seconds nonnegative and timeout positive/finite")
    if not math.isfinite(args.max_rss_growth_mib) or args.max_rss_growth_mib < 0 or args.max_fd_growth < 0:
        parser.error("resource growth limits must be nonnegative and finite")
    for name in ("main", "zone", "localization.txt"):
        if not (args.game_dir / name).exists():
            parser.error(f"missing installed asset path: {args.game_dir / name}")
    if not args.binary.is_file():
        parser.error(f"missing native executable: {args.binary}")
    try:
        args.output_dir.mkdir(parents=True, exist_ok=False)
    except FileExistsError:
        parser.error("--output-dir must not already exist (reports are never overwritten)")
    methods = ("console", "sigterm") if args.shutdown == "both" else (args.shutdown,)
    ok = True
    for method in methods:
        output = args.output_dir / method
        output.mkdir()
        # A separate short run proves SIGTERM ownership/shutdown, not a second soak.
        cycles = args.cycles if method == methods[0] else 0
        ok = run_session(args, output, method, cycles) and ok
        if not ok:
            break
    print(f"Reports: {args.output_dir.resolve()}", flush=True)
    print("No player authenticated/connected; bounded resource stability is not a proof of leak freedom.")
    return 0 if ok else 1


if __name__ == "__main__":
    raise SystemExit(main())
