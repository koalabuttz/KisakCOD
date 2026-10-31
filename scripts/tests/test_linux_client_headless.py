#!/usr/bin/env python3
"""Real loopback UDP/event tests for the standalone discovery diagnostic."""
import argparse
from pathlib import Path
import socket
import subprocess
import threading
import time
import unittest

BINARY = None


class HeadlessTests(unittest.TestCase):
    def start(self, peer, timeout=300):
        process = subprocess.Popen(
            [str(BINARY), "--server", f"127.0.0.1:{peer.getsockname()[1]}",
             "--timeout-ms", str(timeout)], stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
        self.addCleanup(self.stop, process)
        return process

    @staticmethod
    def stop(process):
        if process.poll() is None:
            process.kill()
        process.communicate()

    def peer(self):
        peer = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        peer.bind(("127.0.0.1", 0))
        peer.settimeout(3)
        self.addCleanup(peer.close)
        return peer

    def request(self, peer):
        data, source = peer.recvfrom(2048)
        self.assertTrue(data.startswith(b"\xff" * 4 + b"getinfo "))
        token = data[12:]
        self.assertRegex(token.decode(), r"^[0-9a-f]{16}$")
        return token, source

    def finish(self, process, code, marker):
        stdout, stderr = process.communicate(timeout=4)
        self.assertEqual(process.returncode, code, (stdout, stderr))
        self.assertIn("auth=unavailable connected=false", stdout)
        self.assertIn(marker, stdout)
        self.assertNotIn("connected=true", stdout)

    def test_valid_discovery_without_admission(self):
        peer = self.peer()
        process = self.start(peer)
        token, source = self.request(peer)
        peer.sendto(b"\xff" * 4 + b"infoResponse\n\\challenge\\" + token + b"\\protocol\\1", source)
        self.finish(process, 3, "discovery=received discarded=0 connected=false")
        peer.settimeout(0.05)
        with self.assertRaises(socket.timeout):
            peer.recvfrom(2048)  # No getchallenge/connect follows discovery.

    def test_bad_responses_then_valid(self):
        peer = self.peer()
        process = self.start(peer, 1000)
        token, source = self.request(peer)
        prefix = b"\xff" * 4 + b"infoResponse\n"
        responses = [b"bad marker", prefix + b"\\challenge\\wrong",
                     prefix + b"\\hostname\\test", prefix + b"\\challenge\\" + token + b"\x00junk",
                     prefix + b"\\challenge\\" + token + b"\\challenge\\" + token,
                     prefix + b"\\challenge\\" + token + b"\\padding\\" + b"x" * 2200]
        for response in responses:
            peer.sendto(response, source)
        peer.sendto(prefix + b"\\challenge\\" + token + b"\x00", source)
        self.finish(process, 3, f"discovery=received discarded={len(responses)} connected=false")

    def test_other_source_cannot_complete_discovery(self):
        peer, impostor = self.peer(), self.peer()
        process = self.start(peer)
        token, source = self.request(peer)
        impostor.sendto(b"\xff" * 4 + b"infoResponse\n\\challenge\\" + token, source)
        self.finish(process, 4, "discovery=timeout discarded=0 connected=false")

    def test_silent_peer_times_out(self):
        peer = self.peer()
        process = self.start(peer, 50)
        self.request(peer)
        self.finish(process, 4, "discovery=timeout")

    def test_only_malformed_peer_times_out(self):
        peer = self.peer()
        process = self.start(peer, 100)
        _, source = self.request(peer)
        peer.sendto(b"\xff" * 4 + b"challengeResponse 42", source)
        self.finish(process, 4, "discovery=timeout discarded=1")

    def test_malformed_flood_does_not_extend_deadline(self):
        peer = self.peer()
        process = self.start(peer, 100)
        _, source = self.request(peer)
        stopped = threading.Event()

        def flood():
            while not stopped.is_set():
                peer.sendto(b"invalid discovery response", source)
                stopped.wait(0.002)

        worker = threading.Thread(target=flood)
        worker.start()
        started = time.monotonic()
        try:
            self.finish(process, 4, "discovery=timeout")
            self.assertLess(time.monotonic() - started, 1.0)
        finally:
            stopped.set()
            worker.join()

    def test_no_default_target_and_invalid_arguments(self):
        for args in ([], ["--server", "localhost:1"], ["--server", "127.0.0.1:0"],
                     ["--server", "127.0.0.1:65536"], ["--server", "127.0.0.1:1", "--timeout-ms", "0"],
                     ["--server", "127.0.0.1:1", "--timeout-ms", "10001"], ["--unknown"]):
            with self.subTest(args=args):
                result = subprocess.run([str(BINARY), *args], capture_output=True, text=True, timeout=3)
                self.assertEqual(result.returncode, 2)
                self.assertNotIn("discovery=sent", result.stdout)

    def test_help_needs_no_server(self):
        result = subprocess.run([str(BINARY), "--help"], capture_output=True, text=True, timeout=3)
        self.assertEqual(result.returncode, 0)
        self.assertIn("never authenticates or joins", result.stdout)


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--binary", required=True, type=Path)
    args = parser.parse_args()
    BINARY = args.binary.resolve()
    unittest.main(argv=[__file__])
