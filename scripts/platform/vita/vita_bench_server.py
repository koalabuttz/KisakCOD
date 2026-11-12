#!/usr/bin/env python3
"""The computer's side of the PS Vita test bench.

A Vita with ``bench.txt`` in its data folder asks this server for work when
its multiplayer client starts (docs/vita.md, "A bench that fetches its work";
the Vita's side is src/vita/vita_bench.h). A job is a build's ``eboot.bin``
(or none, for the installed build), a run list and its step files. The Vita
fetches them, runs the list and sends each start's log back. Standard library
only.

    vita_bench_server.py key   --root DIR
        Makes the bench's folder and its key, and prints the line for the
        card's bench.txt.
    vita_bench_server.py add   --root DIR --name NAME --run-list FILE
                               [--vpk FILE | --eboot FILE] [--file FILE ...]
                               [--send NAME ...]
        Queues a job. --vpk takes the eboot.bin out of a build's package.
        The run list goes to the card as runlist-mp.txt, the files under
        their own names (.txt).
    vita_bench_server.py serve --root DIR --bind ADDRESS [--port 28970]
        Answers the Vita until it is stopped. ADDRESS is this computer's
        address on the network the Vita is on.
    vita_bench_server.py list  --root DIR
        What waits and what is done.

The folder: ``key.txt``; ``queue/<job>/`` (``eboot.bin``, ``files/``,
``send.txt``, and ``logs/`` as they arrive); ``done/<job>/`` with
``result.txt`` once the Vita says the job is over.

The key never travels. Every request carries a code made with it
(HMAC-SHA-256) and is refused without one, and the job's description is
signed with a number the Vita picked, so the Vita runs only what the holder
of the key queued. Nothing is encrypted: another device on the same network
can read what passes. Run it on a network you trust, bound to that network's
address, and stop it when the bench is not in use.
"""

import argparse
import hashlib
import hmac
import os
import re
import secrets
import shutil
import sys
import time
import zipfile
from http.server import BaseHTTPRequestHandler, HTTPServer
from pathlib import Path

RUN_LIST = "runlist-mp.txt"
MAX_FILES = 16
MAX_FILE_BYTES = 1 << 20
MAX_EBOOT_BYTES = 64 << 20
MAX_LOG_BYTES = 16 << 20
PLAIN = re.compile(r"^[A-Za-z0-9_][A-Za-z0-9._-]*$")
SHA256 = re.compile(r"^[0-9a-f]{64}$")
NONCE = re.compile(r"^[0-9a-f]{32}$")


def plain_name(name: str, most: int) -> bool:
    return 0 < len(name) < most and bool(PLAIN.match(name))


def job_file_name(name: str) -> bool:
    return plain_name(name, 64) and name.endswith(".txt") and len(name) >= 5 and not name.startswith("bench")


def send_file_name(name: str) -> bool:
    return (plain_name(name, 64) and not name.startswith("bench")
            and name.endswith((".txt", ".log", ".json", ".csv")))


def code(key: str, *parts: bytes) -> str:
    return hmac.new(key.encode(), b"\n".join(parts), hashlib.sha256).hexdigest()


def sha256_file(path: Path) -> str:
    digest = hashlib.sha256()
    with open(path, "rb") as file:
        for block in iter(lambda: file.read(1 << 16), b""):
            digest.update(block)
    return digest.hexdigest()


def read_key(root: Path) -> str:
    key = (root / "key.txt").read_text().strip()
    if not (16 <= len(key) < 48 and key.isalnum() and key.isascii()):
        raise SystemExit("key.txt must hold 16 to 47 letters and digits")
    return key


def queued_jobs(root: Path) -> list:
    queue = root / "queue"
    if not queue.is_dir():
        return []
    return sorted(entry for entry in queue.iterdir() if entry.is_dir() and plain_name(entry.name, 40))


def job_blobs(job: Path) -> dict:
    """What a job is made of: {name on the card or 'eboot': (path, sha256, bytes)}."""
    blobs = {}
    eboot = job / "eboot.bin"
    if eboot.is_file():
        blobs["eboot"] = (eboot, sha256_file(eboot), eboot.stat().st_size)
    files = job / "files"
    if files.is_dir():
        for path in sorted(files.iterdir()):
            if path.is_file() and job_file_name(path.name):
                blobs[path.name] = (path, sha256_file(path), path.stat().st_size)
    return blobs


def job_text(job: Path) -> str:
    lines = [f"job {job.name}"]
    for name, (_, digest, size) in job_blobs(job).items():
        lines.append(f"eboot {digest} {size}" if name == "eboot" else f"file {name} {digest} {size}")
    send = job / "send.txt"
    if send.is_file():
        lines += [f"send {name}" for name in send.read_text().split() if send_file_name(name)]
    return "\n".join(lines) + "\n"


def note(text: str) -> None:
    print(time.strftime("%Y-%m-%d %H:%M:%SZ", time.gmtime()), text, flush=True)


class Handler(BaseHTTPRequestHandler):
    server_version = "KisakCOD-bench"
    timeout = 30

    def log_message(self, format, *args):  # the notes below say more
        pass

    def answer(self, status: int, body: bytes = b"", mac: str = "") -> None:
        self.send_response(status)
        self.send_header("Content-Length", str(len(body)))
        self.send_header("Content-Type", "application/octet-stream")
        if mac:
            self.send_header("X-Bench-Mac", mac)
        self.end_headers()
        self.wfile.write(body)

    def refused(self, why: str) -> None:
        note(f"refused {self.command} {self.path.split('?')[0]} from {self.client_address[0]}: {why}")
        self.answer(403)

    def signed(self, *parts: bytes) -> bool:
        given = self.headers.get("X-Bench-Mac", "")
        return hmac.compare_digest(given, code(self.server.key, *parts))

    def body(self, most: int):
        try:
            length = int(self.headers.get("Content-Length", ""))
        except ValueError:
            return None
        if length < 0 or length > most:
            return None
        data = self.rfile.read(length)
        return data if len(data) == length else None

    def do_GET(self):
        root = self.server.root
        path, _, query = self.path.partition("?")
        if path == "/bench/job":
            nonce = query[len("nonce="):] if query.startswith("nonce=") else ""
            if not NONCE.match(nonce):
                return self.refused("no number")
            if not self.signed(b"ask", nonce.encode()):
                return self.refused("not the key's code")
            jobs = queued_jobs(root)
            text = job_text(jobs[0]) if jobs else "none\n"
            if jobs and self.server.handed != jobs[0].name:
                self.server.handed = jobs[0].name
                note(f"job {jobs[0].name} handed to {self.client_address[0]}")
            data = text.encode()
            return self.answer(200, data, code(self.server.key, b"job", nonce.encode(), data))
        if path.startswith("/bench/blob/"):
            digest = path[len("/bench/blob/"):]
            if not SHA256.match(digest):
                return self.refused("not a file's name")
            if not self.signed(b"blob", digest.encode()):
                return self.refused("not the key's code")
            for job in queued_jobs(root):
                for blob, have, size in job_blobs(job).values():
                    if have == digest:
                        note(f"sending {blob.relative_to(root)} ({size} bytes)")
                        return self.answer(200, blob.read_bytes())
            return self.answer(404)
        return self.answer(404)

    def do_PUT(self):
        root = self.server.root
        match = re.match(r"^/bench/job/([^/]+)/log/([^/]+)$", self.path)
        if not match or not plain_name(match[1], 40):
            return self.answer(404)
        job, name = match[1], match[2]
        if not (send_file_name(name) or name == "bench.log"):
            return self.refused("not a name a log may have")
        data = self.body(MAX_LOG_BYTES)
        if data is None:
            return self.refused("no length, or too long")
        digest = hashlib.sha256(data).hexdigest()
        if not self.signed(b"log", job.encode(), name.encode(), digest.encode()):
            return self.refused("not the key's code")
        folder = root / "queue" / job
        if not folder.is_dir():
            return self.answer(404)
        (folder / "logs").mkdir(exist_ok=True)
        (folder / "logs" / name).write_bytes(data)
        note(f"job {job}: received {name} ({len(data)} bytes)")
        return self.answer(200)

    def do_POST(self):
        root = self.server.root
        match = re.match(r"^/bench/job/([^/]+)/done$", self.path)
        if not match or not plain_name(match[1], 40):
            return self.answer(404)
        job = match[1]
        data = self.body(1024)
        if data is None:
            return self.refused("no length, or too long")
        if not self.signed(b"done", job.encode(), data):
            return self.refused("not the key's code")
        folder = root / "queue" / job
        done = root / "done" / job
        if not folder.is_dir():
            return self.answer(200 if done.is_dir() else 404)
        (folder / "result.txt").write_bytes(data + b"\n")
        (root / "done").mkdir(exist_ok=True)
        if done.exists():
            shutil.rmtree(done)
        folder.rename(done)
        note(f"job {job} is over: {data.decode(errors='replace')}")
        return self.answer(200)


def command_key(args) -> int:
    root = Path(args.root)
    (root / "queue").mkdir(parents=True, exist_ok=True)
    (root / "done").mkdir(exist_ok=True)
    key_file = root / "key.txt"
    if not key_file.exists():
        descriptor = os.open(key_file, os.O_WRONLY | os.O_CREAT | os.O_EXCL, 0o600)
        with os.fdopen(descriptor, "w") as file:
            file.write(secrets.token_hex(16) + "\n")
    key = read_key(root)
    print(f"The card's ux0:data/KisakCOD/bench.txt, one line (put this computer's address in):")
    print(f"<address> {args.port} {key}")
    return 0


def command_add(args) -> int:
    root = Path(args.root)
    read_key(root)
    if not plain_name(args.name, 20):
        raise SystemExit("--name: up to 19 letters, digits, '.', '_' and '-'")
    job_id = time.strftime("%Y%m%dT%H%M%S", time.gmtime()) + "-" + args.name
    files = {RUN_LIST: Path(args.run_list)}
    for given in args.file or []:
        path = Path(given)
        if not job_file_name(path.name) or path.name == RUN_LIST:
            raise SystemExit(f"--file {given}: a file's name must be plain, end in .txt and not start with bench")
        if path.name in files:
            raise SystemExit(f"--file {given}: named twice")
        files[path.name] = path
    if len(files) > MAX_FILES:
        raise SystemExit(f"a job holds up to {MAX_FILES} files")
    for name, path in files.items():
        if not path.is_file() or path.stat().st_size > MAX_FILE_BYTES:
            raise SystemExit(f"{path}: not a file of up to 1 MB")
    starts = 0
    for number, line in enumerate(files[RUN_LIST].read_text().splitlines(), 1):
        line = line.strip()
        if not line or line.startswith("#"):
            continue
        if line.count("|") < 2:
            raise SystemExit(f"{files[RUN_LIST]} line {number}: expected <name> | <markers or -> | <command line>")
        starts += 1
    if not starts:
        raise SystemExit(f"{files[RUN_LIST]}: no start")
    for name in args.send or []:
        if not send_file_name(name):
            raise SystemExit(f"--send {name}: a plain name ending in .txt, .log, .json or .csv")

    eboot = None
    if args.vpk:
        with zipfile.ZipFile(args.vpk) as package:
            eboot = package.read("eboot.bin")
    elif args.eboot:
        eboot = Path(args.eboot).read_bytes()
    if eboot is not None and not 0 < len(eboot) <= MAX_EBOOT_BYTES:
        raise SystemExit("the eboot.bin is empty or over 64 MB")

    # Made under a name the server skips, then renamed: a job is never
    # handed out half written.
    work = root / "queue" / f".new-{job_id}"
    (work / "files").mkdir(parents=True)
    for name, path in files.items():
        shutil.copyfile(path, work / "files" / name)
    if eboot is not None:
        (work / "eboot.bin").write_bytes(eboot)
    if args.send:
        (work / "send.txt").write_text("\n".join(args.send) + "\n")
    work.rename(root / "queue" / job_id)
    build = f"its own build ({hashlib.sha256(eboot).hexdigest()[:12]})" if eboot is not None else "the installed build"
    print(f"queued {job_id}: {starts} starts on {build}, {len(files)} files")
    return 0


def command_list(args) -> int:
    root = Path(args.root)
    for job in queued_jobs(root):
        logs = sorted(path.name for path in (job / "logs").iterdir()) if (job / "logs").is_dir() else []
        print(f"waits  {job.name}" + (f"  (logs so far: {', '.join(logs)})" if logs else ""))
    done = root / "done"
    for job in sorted(done.iterdir()) if done.is_dir() else []:
        result = (job / "result.txt").read_text().strip() if (job / "result.txt").is_file() else "?"
        print(f"done   {job.name}  {result}")
    return 0


def command_serve(args) -> int:
    root = Path(args.root).resolve()
    server = HTTPServer((args.bind, args.port), Handler)
    server.root = root
    server.key = read_key(root)
    server.handed = None
    note(f"listening on {server.server_address[0]}:{server.server_address[1]}, folder {root}")
    try:
        server.serve_forever()
    except KeyboardInterrupt:
        pass
    return 0


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    commands = parser.add_subparsers(dest="command", required=True)
    for name in ("key", "add", "serve", "list"):
        sub = commands.add_parser(name)
        sub.add_argument("--root", required=True)
        if name in ("key", "serve"):
            sub.add_argument("--port", type=int, default=28970)
        if name == "serve":
            sub.add_argument("--bind", required=True)
        if name == "add":
            sub.add_argument("--name", required=True)
            sub.add_argument("--run-list", required=True)
            build = sub.add_mutually_exclusive_group()
            build.add_argument("--vpk")
            build.add_argument("--eboot")
            sub.add_argument("--file", action="append")
            sub.add_argument("--send", action="append")
    args = parser.parse_args()
    return {"key": command_key, "add": command_add, "serve": command_serve, "list": command_list}[args.command](args)


if __name__ == "__main__":
    sys.exit(main())
