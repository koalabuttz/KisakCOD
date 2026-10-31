#!/usr/bin/env python3
"""Unit tests for the native lifecycle driver's pure logic and failure handling.

These tests never spawn the dedicated server and never touch licensed assets.
They drive the driver module imported from this directory with stdlib mocks:
reply parsing, resource deltas, the shutdown log-window check, constructor
teardown when the client socket cannot bind, and run_session's reporting and
cleanup ordering.

Run with: python3 scripts/tests/test_linux_lifecycle_driver.py
"""
import contextlib
import io
import json
from pathlib import Path
import signal
import subprocess
import tempfile
import types
import unittest
from unittest import mock

import test_linux_server_lifecycle as driver

MiB = 1024 * 1024


def reply(kind, body):
    """Build a query reply packet: the OOB marker, the type tag, and the body."""
    return driver.OOB + kind.encode("ascii") + b"\n" + body


def fields(*pairs):
    """Build a backslash-delimited keyword body from (key, value) pairs."""
    return "".join("\\%s\\%s" % pair for pair in pairs).encode("utf-8")


class ParseReplyTest(unittest.TestCase):
    """parse_reply must reject every stale or malformed reply, not just some."""

    def test_accepts_getinfo_reply(self):
        body = fields(("protocol", "1"), ("challenge", "abc123"),
                      ("mapname", "mp_crash"), ("hostname", "kisak-test-1"),
                      ("clients", "0"), ("sv_maxclients", "18"))
        self.assertEqual(
            driver.parse_reply(reply("infoResponse", body), "infoResponse", "abc123"),
            {"protocol": "1", "challenge": "abc123", "mapname": "mp_crash",
             "hostname": "kisak-test-1", "clients": "0", "sv_maxclients": "18"})

    def test_accepts_getstatus_reply(self):
        body = fields(("challenge", "deadbeef"), ("mapname", "mp_shipment"),
                      ("sv_hostname", "kisak-test-2"), ("gametype", "war"))
        self.assertEqual(
            driver.parse_reply(reply("statusResponse", body), "statusResponse", "deadbeef"),
            {"challenge": "deadbeef", "mapname": "mp_shipment",
             "sv_hostname": "kisak-test-2", "gametype": "war"})

    def test_trailing_nulls_and_crlf_are_trimmed(self):
        body = fields(("challenge", "c0ffee"), ("mapname", "mp_crash")) + b"\r\x00\x00\r"
        self.assertEqual(driver.parse_reply(reply("infoResponse", body), "infoResponse", "c0ffee"),
                         {"challenge": "c0ffee", "mapname": "mp_crash"})

    def test_rejects_wrong_out_of_band_marker(self):
        body = fields(("challenge", "abc123"), ("mapname", "mp_crash"))
        for oob in (b"", b"\xff\xff\xff", b"\xff\xff\xff\xff\xff", b"\xfe\xfe\xfe\xfe"):
            self.assertIsNone(driver.parse_reply(oob + b"infoResponse\n" + body,
                                                 "infoResponse", "abc123"), oob)

    def test_rejects_wrong_kind(self):
        body = fields(("challenge", "abc123"), ("mapname", "mp_crash"))
        packet = reply("statusResponse", body)
        self.assertIsNone(driver.parse_reply(packet, "infoResponse", "abc123"))
        self.assertIsNone(driver.parse_reply(reply("infoResponse", body), "statusResponse", "abc123"))
        self.assertIsNone(driver.parse_reply(reply("infoResponsex", body), "infoResponse", "abc123"))

    def test_rejects_wrong_or_missing_challenge(self):
        body = fields(("challenge", "abc123"), ("mapname", "mp_crash"))
        packet = reply("infoResponse", body)
        self.assertIsNone(driver.parse_reply(packet, "infoResponse", "abc124"))
        self.assertIsNone(driver.parse_reply(packet, "infoResponse", ""))
        missing = reply("infoResponse", fields(("mapname", "mp_crash")))
        self.assertIsNone(driver.parse_reply(missing, "infoResponse", "abc123"))

    def test_rejects_invalid_utf8(self):
        body = b"\\challenge\\abc123\\mapname\\m\xffp\r\n"
        self.assertIsNone(driver.parse_reply(reply("infoResponse", body), "infoResponse", "abc123"))
        # A stray continuation byte alone must also defeat decoding.
        body = b"\\challenge\\\x80\x80\\mapname\\mp_crash"
        self.assertIsNone(driver.parse_reply(reply("infoResponse", body), "infoResponse", "\u0080\u0080"))

    def test_rejects_duplicate_keys(self):
        body = fields(("challenge", "abc123"), ("mapname", "mp_crash"), ("mapname", "mp_shipment"))
        self.assertIsNone(driver.parse_reply(reply("infoResponse", body), "infoResponse", "abc123"))
        dup_challenge = fields(("challenge", "abc123"), ("challenge", "abc123"))
        self.assertIsNone(driver.parse_reply(reply("infoResponse", dup_challenge),
                                             "infoResponse", "abc123"))

    def test_rejects_odd_key_value_count(self):
        # A trailing key with no value leaves an even number of segments.
        self.assertIsNone(driver.parse_reply(reply("infoResponse", b"\\challenge"),
                                             "infoResponse", ""))
        body = b"\\challenge\\abc123\\mapname"
        self.assertIsNone(driver.parse_reply(reply("infoResponse", body), "infoResponse", "abc123"))

    def test_rejects_body_not_starting_with_separator(self):
        body = b"challenge\\abc123\\mapname\\mp_crash"
        self.assertIsNone(driver.parse_reply(reply("infoResponse", body), "infoResponse", "abc123"))

    def test_rejects_empty_key(self):
        body = b"\\\\\\challenge\\abc123"
        self.assertIsNone(driver.parse_reply(reply("infoResponse", body), "infoResponse", "abc123"))


class ResourceGrowthTest(unittest.TestCase):
    """Growth is a plain later-minus-baseline delta on the reported counters."""

    def test_positive_growth(self):
        baseline = {"rss_bytes": 100, "fds": 4, "threads": 2}
        later = {"rss_bytes": 250, "fds": 9, "threads": 5}
        self.assertEqual(driver.resource_growth(baseline, later),
                         {"rss_bytes": 150, "fds": 5, "threads": 3})

    def test_negative_growth(self):
        baseline = {"rss_bytes": 250, "fds": 9, "threads": 5}
        later = {"rss_bytes": 100, "fds": 4, "threads": 2}
        self.assertEqual(driver.resource_growth(baseline, later),
                         {"rss_bytes": -150, "fds": -5, "threads": -3})


class ShutdownTest(unittest.TestCase):
    """Server.shutdown judges only the log written after it started watching."""

    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory(prefix="kisak-lifecycle-unit-")
        self.addCleanup(self.temporary.cleanup)
        self.directory = Path(self.temporary.name)

    def make_server(self, method="console", exit_code=0):
        server = driver.Server.__new__(driver.Server)
        server.log_path = self.directory / "server.log"
        server.log_path.write_bytes(b"")
        server.timeout = 5.0
        server.output = self.directory
        server.command = ["fake-dedi"]
        server.send = mock.Mock()
        server.alive = mock.Mock()
        server.process = mock.Mock()
        server.process.wait.return_value = exit_code
        server.process.returncode = exit_code
        server.method = method
        return server

    def run_shutdown(self, server, stale=b"", fresh=b""):
        """Append *fresh* while the driver is watching, after a stale prefix."""
        server.log_path.write_bytes(stale)

        def append():
            with server.log_path.open("ab") as stream:
                stream.write(fresh)

        if server.method == "console":
            server.send.side_effect = lambda command: append()
        else:
            server.alive.side_effect = append
        out = io.StringIO()
        with contextlib.redirect_stdout(out):
            server.shutdown(server.method)
        return out.getvalue()

    def run_shutdown_failing(self, server, stale=b"", fresh=b""):
        with self.assertRaises(driver.CheckFailure) as caught:
            self.run_shutdown(server, stale, fresh)
        return str(caught.exception)

    def test_console_shutdown_requires_fresh_ordered_cleanup(self):
        server = self.make_server("console")
        output = self.run_shutdown(server, fresh=b"quitting...\nShutdownGame: done\n")
        server.send.assert_called_once_with("quit")
        self.assertIn("PASS: console shutdown", output)

    def test_console_shutdown_fails_with_quit_only(self):
        server = self.make_server("console")
        error = self.run_shutdown_failing(server, fresh=b"quitting...\nbye\n")
        self.assertIn("missing fresh ordered game cleanup", error)

    def test_console_shutdown_fails_when_cleanup_precedes_quit(self):
        server = self.make_server("console")
        error = self.run_shutdown_failing(server, fresh=b"ShutdownGame: done\nquitting...\n")
        self.assertIn("missing fresh ordered game cleanup", error)

    def test_stale_cleanup_before_the_window_does_not_count(self):
        # An earlier run's cleanup in the log must not excuse a silent shutdown.
        server = self.make_server("console")
        error = self.run_shutdown_failing(server,
                                          stale=b"quitting...\nShutdownGame: earlier\n",
                                          fresh=b"quitting...\n")
        self.assertIn("missing fresh ordered game cleanup", error)

    def test_nonzero_exit_fails(self):
        server = self.make_server("console", exit_code=1)
        error = self.run_shutdown_failing(server, fresh=b"quitting...\nShutdownGame: done\n")
        self.assertIn("exit 1", error)

    def test_assert_in_fresh_window_fails(self):
        server = self.make_server("console")
        error = self.run_shutdown_failing(
            server, fresh=b"quitting...\nASSERT FAILED: nope\nShutdownGame: done\n")
        self.assertIn("error during console shutdown", error)

    def test_sigterm_shutdown_accepts_ordered_cleanup(self):
        server = self.make_server("sigterm")
        output = self.run_shutdown(server, fresh=b"quitting...\nShutdownGame: done\n")
        server.alive.assert_called_once_with()
        server.process.send_signal.assert_called_once_with(signal.SIGTERM)
        server.send.assert_not_called()
        self.assertIn("PASS: sigterm shutdown", output)

    def test_sigterm_shutdown_fails_without_fresh_cleanup(self):
        server = self.make_server("sigterm")
        error = self.run_shutdown_failing(server, fresh=b"quitting...\n")
        self.assertIn("missing fresh ordered game cleanup", error)

    def test_shutdown_timeout_fails(self):
        server = self.make_server("console")
        server.process.wait.side_effect = subprocess.TimeoutExpired(["fake-dedi"], 5)
        error = self.run_shutdown_failing(server, fresh=b"quitting...\nShutdownGame: done\n")
        self.assertIn("console shutdown timed out", error)


class _Reservation:
    """Stands in for the throwaway socket that reserves a loopback port."""

    def __init__(self):
        self.bound = None

    def __enter__(self):
        return self

    def __exit__(self, *exception_info):
        return False

    def bind(self, address):
        self.bound = address

    def getsockname(self):
        return ("127.0.0.1", 45678)


class _UnbindableClient:
    """A client socket whose bind fails, as when the reserved port is taken."""

    def __init__(self, error):
        self.error = error
        self.closed = False

    def bind(self, address):
        raise self.error

    def close(self):
        self.closed = True


class ConstructorFailureTest(unittest.TestCase):
    """A failed construction must not leak the child, its stdin, or the log."""

    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory(prefix="kisak-lifecycle-unit-")
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name)
        self.work = self.root / "work"
        self.work.mkdir()
        self.output = self.root / "output"
        self.output.mkdir()
        self.game = self.root / "game"
        for name in ("main", "zone"):
            (self.game / name).mkdir(parents=True)
        (self.game / "localization.txt").write_text("")
        self.binary = self.game / "KisakCOD-dedi"

    def test_client_socket_bind_failure_cleans_child_and_log(self):
        reservation = _Reservation()
        client = _UnbindableClient(OSError(98, "Address already in use"))
        calls = []

        def socket_factory(*arguments, **keywords):
            calls.append((arguments, keywords))
            return reservation if len(calls) == 1 else client

        child = mock.Mock()
        child.poll.return_value = None
        child.stdin = mock.Mock()

        with mock.patch.object(driver.socket, "socket", side_effect=socket_factory), \
                mock.patch.object(driver.subprocess, "Popen", return_value=child) as popen:
            with self.assertRaises(OSError):
                driver.Server(self.binary, self.game, self.work, self.output, 30.0)

        popen.assert_called_once()
        command = popen.call_args.args[0]
        self.assertEqual(command[0], str(self.binary))
        self.assertIn("net_port", command)
        self.assertEqual(command[command.index("net_port") + 1], "45678")
        self.assertEqual(popen.call_args.kwargs["cwd"], self.work / "home")
        self.assertEqual(popen.call_args.kwargs["stdin"], subprocess.PIPE)
        log_stream = popen.call_args.kwargs["stdout"]
        self.assertEqual(log_stream.name, str(self.output / "server.log"))

        child.kill.assert_called_once_with()
        child.wait.assert_called_once_with()
        child.stdin.close.assert_called_once_with()
        self.assertTrue(log_stream.closed, "the log stream must be closed on failure")

        self.assertEqual(len(calls), 2, "reserve a port, then open the client socket")
        self.assertEqual(reservation.bound, ("127.0.0.1", 0))
        self.assertEqual((self.work / "assets" / "main").resolve(), (self.game / "main").resolve())
        self.assertEqual((self.work / "home" / "localization.txt").resolve(),
                         (self.game / "localization.txt").resolve())


class _FakeServer:
    """Records the driver's calls; never spawns a process."""

    instances = []

    def __init__(self, binary, game, work, output, timeout):
        self.binary = binary
        self.game = game
        self.work = Path(work)
        self.output = output
        self.timeout = timeout
        self.home = self.work / "home"
        self.home.mkdir()
        self.work_exists_at_init = self.work.is_dir()
        self.command = ["fake-dedi", "+set", "dedicated", "1"]
        self.stages = []
        self.samples = []
        self.cycle_endpoints = []
        self.process = types.SimpleNamespace(returncode=0, pid=4242)
        self.transitions = []
        self.sent = []
        self.shutdowns = []
        self.cleaned = False
        self.home_exists_at_cleanup = None
        self.work_exists_at_cleanup = None
        _FakeServer.instances.append(self)

    def transition(self, label, command, map_name, hold_seconds):
        self.transitions.append((label, command, map_name, hold_seconds))
        self.fail_if_needed(label)
        snapshot = self.snapshot(len(self.transitions) - 1)
        self.stages.append({"label": label, "command": command, "map": map_name,
                            "sample": snapshot})
        return snapshot

    def send(self, command):
        self.sent.append(command)

    def shutdown(self, method):
        self.fail_if_needed("shutdown")
        self.shutdowns.append(method)
        self.process.returncode = 0

    def cleanup(self):
        self.cleaned = True
        self.home_exists_at_cleanup = self.home.exists()
        self.work_exists_at_cleanup = self.work.exists()

    def fail_if_needed(self, label):
        plan = _FakeServer.plan
        if plan is not None and plan.get("label") == label:
            raise plan["error"]

    def snapshot(self, index):
        plan = _FakeServer.snapshots
        if not plan:
            return {"monotonic": float(index), "rss_bytes": 1000, "fds": 4, "threads": 2}
        return dict(plan[min(index, len(plan) - 1)])


class RunSessionTest(unittest.TestCase):
    """run_session always writes a truthful report and never outlives its temp home."""

    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory(prefix="kisak-lifecycle-unit-")
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name)
        self.output = self.root / "output"
        self.output.mkdir()
        _FakeServer.instances = []
        _FakeServer.plan = None
        _FakeServer.snapshots = None
        patcher = mock.patch.object(driver, "Server", _FakeServer)
        patcher.start()
        self.addCleanup(patcher.stop)
        quiet = mock.patch("sys.stdout", io.StringIO())
        quiet.start()
        self.addCleanup(quiet.stop)
        self.args = types.SimpleNamespace(binary=self.root / "KisakCOD-dedi",
                                          game_dir=self.root / "game",
                                          timeout=30.0, hold_seconds=0.0,
                                          max_rss_growth_mib=64.0, max_fd_growth=2)

    def report(self):
        return json.loads((self.output / "report.json").read_text())

    def test_success_marks_the_report_passed(self):
        self.assertTrue(driver.run_session(self.args, self.output, "console", 1))
        report = self.report()
        self.assertTrue(report["passed"])
        self.assertIsNone(report["error"])
        self.assertEqual(report["shutdown"], "console")
        self.assertEqual(report["exit_code"], 0)
        self.assertEqual(report["command"], ["fake-dedi", "+set", "dedicated", "1"])
        self.assertTrue(report["stages"])
        self.assertEqual(len(report["cycle_endpoints"]), 1)
        server = _FakeServer.instances[0]
        self.assertEqual(server.shutdowns, ["console"])
        self.assertIn('set sv_mapRotation "map mp_crash map mp_shipment"', server.sent)
        self.assertIn('set sv_mapRotationCurrent ""', server.sent)
        self.assertEqual([entry[0] for entry in server.transitions],
                         ["initial map", "cycle 1 restart", "cycle 1 map change", "cycle 1 rotation"])
        self.assertEqual([entry[1] for entry in server.transitions],
                         [None, "map_restart", "map mp_shipment", "map_rotate"])
        self.assertTrue(server.cleaned)

    def test_check_failure_is_reported_and_returns_false(self):
        _FakeServer.plan = {"label": "initial map", "error": driver.CheckFailure("boom")}
        self.assertFalse(driver.run_session(self.args, self.output, "console", 1))
        report = self.report()
        self.assertFalse(report["passed"])
        self.assertEqual(report["error"], "boom")
        self.assertNotIn("exit_code", report)
        server = _FakeServer.instances[0]
        self.assertTrue(server.cleaned)
        self.assertEqual(server.shutdowns, [])

    def test_base_exception_is_reported_and_reraised(self):
        _FakeServer.plan = {"label": "cycle 1 restart", "error": KeyboardInterrupt("ctrl-c")}
        with self.assertRaises(KeyboardInterrupt):
            driver.run_session(self.args, self.output, "sigterm", 1)
        report = self.report()
        self.assertFalse(report["passed"])
        self.assertEqual(report["error"], "KeyboardInterrupt: ctrl-c")
        self.assertTrue(_FakeServer.instances[0].cleaned)

    def test_cleanup_runs_while_the_temporary_home_still_exists(self):
        _FakeServer.plan = {"label": "initial map", "error": KeyboardInterrupt("stop")}
        with self.assertRaises(KeyboardInterrupt):
            driver.run_session(self.args, self.output, "console", 1)
        server = _FakeServer.instances[0]
        self.assertTrue(server.work_exists_at_init)
        self.assertTrue(server.home_exists_at_cleanup,
                        "the child must be stopped before its writable home is removed")
        self.assertTrue(server.work_exists_at_cleanup)
        self.assertFalse(server.work.exists(), "the temporary directory is removed afterwards")

    def test_resource_growth_within_limits_passes(self):
        _FakeServer.snapshots = [
            {"monotonic": 0.0, "rss_bytes": 100 * MiB, "fds": 10, "threads": 8},
            {"monotonic": 1.0, "rss_bytes": 101 * MiB, "fds": 10, "threads": 8},
            {"monotonic": 2.0, "rss_bytes": 102 * MiB, "fds": 10, "threads": 8},
            {"monotonic": 3.0, "rss_bytes": 100 * MiB, "fds": 10, "threads": 8},
            {"monotonic": 4.0, "rss_bytes": 120 * MiB, "fds": 11, "threads": 8},
            {"monotonic": 5.0, "rss_bytes": 150 * MiB, "fds": 11, "threads": 8},
            {"monotonic": 6.0, "rss_bytes": 163 * MiB, "fds": 12, "threads": 8},
        ]
        self.assertTrue(driver.run_session(self.args, self.output, "console", 2))
        report = self.report()
        self.assertEqual(report["growth_after_warmup"],
                         [{"rss_bytes": 63 * MiB, "fds": 2, "threads": 0}])

    def test_resource_growth_over_limit_fails(self):
        _FakeServer.snapshots = [
            {"monotonic": 0.0, "rss_bytes": 100 * MiB, "fds": 10, "threads": 8},
            {"monotonic": 1.0, "rss_bytes": 100 * MiB, "fds": 10, "threads": 8},
            {"monotonic": 2.0, "rss_bytes": 100 * MiB, "fds": 10, "threads": 8},
            {"monotonic": 3.0, "rss_bytes": 100 * MiB, "fds": 10, "threads": 8},
            {"monotonic": 4.0, "rss_bytes": 100 * MiB, "fds": 10, "threads": 8},
            {"monotonic": 5.0, "rss_bytes": 100 * MiB, "fds": 10, "threads": 8},
            {"monotonic": 6.0, "rss_bytes": 165 * MiB, "fds": 10, "threads": 8},
        ]
        self.assertFalse(driver.run_session(self.args, self.output, "console", 2))
        report = self.report()
        self.assertFalse(report["passed"])
        self.assertIn("resource growth exceeded thresholds", report["error"])
        self.assertEqual(_FakeServer.instances[0].shutdowns, [])

    def test_growth_is_skipped_for_a_single_cycle(self):
        self.assertTrue(driver.run_session(self.args, self.output, "console", 1))
        self.assertNotIn("growth_after_warmup", self.report())


class TransitionDeadlineTest(unittest.TestCase):
    def test_restart_queries_share_the_full_transition_budget(self):
        server = driver.Server.__new__(driver.Server)
        server.timeout = 60
        server.stages = []
        server.log_path = mock.Mock()
        server.log_path.stat.return_value.st_size = 0
        server.log_path.read_bytes.return_value = b"Server: mp_crash\nmap: mp_crash\n"
        server.send = mock.Mock()
        server.query = mock.Mock()
        server.hold = mock.Mock()
        server.sample = mock.Mock(return_value={})
        # Three seconds passed before getinfo; ten seconds before getstatus.
        with mock.patch.object(driver.time, "monotonic", side_effect=[100, 103, 109, 110]):
            server.transition("restart", "map_restart", "mp_crash", 2)
        self.assertEqual(server.query.call_args_list, [
            mock.call("getinfo", "mp_crash", timeout=57),
            mock.call("getstatus", "mp_crash", timeout=50)])
        server.hold.assert_called_once_with("mp_crash", 2)

    def test_query_failure_still_fails_transition(self):
        server = driver.Server.__new__(driver.Server)
        server.timeout = 60
        server.stages = []
        server.log_path = mock.Mock()
        server.log_path.stat.return_value.st_size = 0
        server.send = mock.Mock()
        server.query = mock.Mock(side_effect=driver.CheckFailure("unexpected query protocol"))
        server.hold = mock.Mock()
        with self.assertRaisesRegex(driver.CheckFailure, "unexpected query protocol"):
            server.transition("restart", "map_restart", "mp_crash", 2)
        server.hold.assert_not_called()
        self.assertEqual(server.stages, [])


if __name__ == "__main__":
    unittest.main(verbosity=2)
