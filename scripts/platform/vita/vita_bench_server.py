#!/usr/bin/env python3
"""The computer's side of the PS Vita test bench.

A Vita with ``bench.txt`` in its data folder asks this server for work when
its multiplayer client starts (docs/vita.md, "A bench that fetches its work";
the Vita's side is src/vita/vita_bench.h). A job is a build (or none, for the
installed client), a run list and its step files. The Vita fetches them, puts
the build in its second application, KisakCOD Test (installing that when it
is not there), runs the list and sends each start's log back. Standard
library only.

    vita_bench_server.py key   --root DIR
        Makes the bench's folder and its key, and prints the line for the
        card's bench.txt.
    vita_bench_server.py head-template --root DIR [--from FILE]
        Fetches the package head that the Vita's installer needs (VitaShell's
        resources/head.bin, at a fixed commit and checked by its SHA-256),
        or takes it from FILE. Once; without it a job can only replace the
        build of a test title that is installed already.
    vita_bench_server.py add   --root DIR --name NAME --run-list FILE
                               [--vpk FILE | --eboot FILE | --ci-commit SHA]
                               [--file FILE ...] [--pack FILE] [--send NAME ...]
        Queues a job. --vpk is the build's KisakCOD-mp-test.vpk (the test
        title's package, which CI builds beside KisakCOD-mp.vpk): its
        eboot.bin, and its param.sfo and a head made for it, so the Vita can
        install the test title. --eboot is a bare eboot.bin. --ci-commit
        fetches the commit's KisakCOD-mp-test.vpk from hosted CI with gh
        (the workflow "PS Vita server and client"; it must have finished
        green) into ci/<sha>/ and queues that.
        The run list goes to the card as runlist-mp.txt, the files under
        their own names (.txt). --pack is a programs.pack of compiled
        programs (vita_pack_programs.py) for the card's program folder.
    vita_bench_server.py serve --root DIR --bind ADDRESS [--port 28970]
        Answers the Vita until it is stopped. ADDRESS is this computer's
        address on the network the Vita is on.
    vita_bench_server.py list  --root DIR
        What waits and what is done.

The folder: ``key.txt``; ``head-template.bin``; ``queue/<job>/``
(``eboot.bin``, ``param.sfo``, ``head.bin``, ``programs.pack``, ``files/``,
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
import json
import struct
import subprocess
import time
import urllib.request
import zipfile
from http.server import BaseHTTPRequestHandler, HTTPServer
from pathlib import Path

RUN_LIST = "runlist-mp.txt"
MAX_FILES = 16
MAX_FILE_BYTES = 1 << 20
MAX_EBOOT_BYTES = 64 << 20
MAX_PACK_BYTES = 64 << 20
MAX_LOG_BYTES = 16 << 20
MAX_SFO_BYTES = 8 << 10
TEST_TITLE = "KSKD00004"
CI_REPO = "koalabuttz/KisakCOD"
CI_WORKFLOW = "build-kisakcod-vita.yaml"
CI_ARTIFACT = "KisakCOD-mp-vita-RelWithDebInfo"
# The package head VitaShell installs homebrew with (GPLv3, as this project):
# a fixed commit, checked by its SHA-256.
HEAD_TEMPLATE_URL = ("https://raw.githubusercontent.com/TheOfficialFloW/VitaShell/"
                     "81af70971ba18b8ce86215b04180f1e3d21cdfc9/resources/head.bin")
HEAD_TEMPLATE_SHA256 = "cbb88299319048e19115a1fc9c76b04e33745291749de0e08963b9b425623f45"
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


def sfo_text(data: bytes, key: str):
    """A text value of a param.sfo, or None (src/vita/vita_bench.h, SfoText)."""
    if len(data) < 20 or data[:4] != b"\0PSF":
        return None
    keys, values, count = struct.unpack_from("<III", data, 8)
    if keys > len(data) or values > len(data) or count > (len(data) - 20) // 16:
        return None
    for index in range(count):
        key_at, form, length, _, value_at = struct.unpack_from("<HHIII", data, 20 + index * 16)
        key_at += keys
        value_at += values
        end = data.find(b"\0", key_at)
        if key_at >= len(data) or end < 0 or data[key_at:end] != key.encode():
            continue
        if form != 0x0204 or value_at + length > len(data) or not length:
            return None
        return data[value_at:value_at + length].split(b"\0")[0].decode(errors="replace") or None
    return None


def fpkg_hmac(data: bytes) -> bytes:
    """The code a homebrew package's head carries in place of Sony's (after
    VitaShell's package_installer.c, fpkg_hmac)."""
    sha = hashlib.sha1(data).digest()
    buffer = bytearray(64)
    buffer[0:8] = sha[4:12]
    buffer[8:16] = sha[4:12]
    buffer[16:20] = sha[12:16]
    buffer[20] = sha[16]
    buffer[21:24] = sha[1:4]
    buffer[24:32] = buffer[16:24]
    return hashlib.sha1(bytes(buffer)).digest()[:16]


def make_head(template: bytes, sfo: bytes) -> bytes:
    """sce_sys/package/head.bin for a homebrew package (after VitaShell's
    makeHeadBin): the template with the content id and three codes."""
    title = sfo_text(sfo, "TITLE_ID")
    if not title or len(title) != 9 or title != title.upper():
        raise SystemExit("the param.sfo has no title id of nine capitals and digits")
    content = sfo_text(sfo, "CONTENT_ID") or f"EP9000-{title}_00-0000000000000000"
    head = bytearray(template)
    head[0x30:0x30 + 48] = content.encode()[:48].ljust(48, b"\0")
    big = lambda at: struct.unpack_from(">I", head, at)[0]
    for start, length, out in ((0, big(0xD0), big(0xD0)),
                               (big(0x8), big(0x10) - 64, big(0xD4)),
                               (0, big(0xE8), big(0xE8))):
        if not (0 <= start and start + length <= len(head) and out + 16 <= len(head)):
            raise SystemExit("head-template.bin is not a package head")
        head[out:out + 16] = fpkg_hmac(bytes(head[start:start + length]))
    return bytes(head)


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
    """What a job is made of: {name on the card, 'eboot' or 'pack': (path, sha256, bytes)}."""
    blobs = {}
    for kind, name in (("eboot", "eboot.bin"), ("pack", "programs.pack"), ("sfo", "param.sfo"),
                       ("head", "head.bin")):
        path = job / name
        if path.is_file():
            blobs[kind] = (path, sha256_file(path), path.stat().st_size)
    files = job / "files"
    if files.is_dir():
        for path in sorted(files.iterdir()):
            if path.is_file() and job_file_name(path.name):
                blobs[path.name] = (path, sha256_file(path), path.stat().st_size)
    return blobs


def job_text(job: Path) -> str:
    lines = [f"job {job.name}"]
    for name, (_, digest, size) in job_blobs(job).items():
        lines.append(f"{name} {digest} {size}" if name in ("eboot", "pack", "sfo", "head")
                     else f"file {name} {digest} {size}")
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
        # Only a job that waits takes logs, and that is known before the
        # body is read: the code covers the body, so a stranger's upload is
        # read before it can be refused, and this keeps that to the one job.
        folder = root / "queue" / job
        if not folder.is_dir():
            return self.answer(404)
        if not SHA256.match(self.headers.get("X-Bench-Mac", "")):
            return self.refused("no code")
        data = self.body(MAX_LOG_BYTES)
        if data is None:
            return self.refused("no length, or too long")
        digest = hashlib.sha256(data).hexdigest()
        if not self.signed(b"log", job.encode(), name.encode(), digest.encode()):
            return self.refused("not the key's code")
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


def ci_vpk(root: Path, commit: str, repo: str) -> Path:
    """The commit's test .vpk from hosted CI, fetched once into ci/<sha>/."""
    if not re.match(r"^[0-9a-f]{7,40}$", commit):
        raise SystemExit("--ci-commit: a commit's hex name")
    runs = subprocess.run(["gh", "run", "list", "--repo", repo, "--commit", commit, "--workflow", CI_WORKFLOW,
                           "--json", "databaseId,status,conclusion,headSha"],
                          capture_output=True, text=True, timeout=120)
    if runs.returncode:
        raise SystemExit(f"gh run list: {runs.stderr.strip()}")
    green = [run for run in json.loads(runs.stdout or "[]")
             if run.get("status") == "completed" and run.get("conclusion") == "success"]
    if not green:
        raise SystemExit(f"no finished green {CI_WORKFLOW} run of {commit} yet")
    folder = root / "ci" / green[0]["headSha"]
    vpk = folder / "KisakCOD-mp-test.vpk"
    if not vpk.is_file():
        folder.mkdir(parents=True, exist_ok=True)
        fetched = subprocess.run(["gh", "run", "download", str(green[0]["databaseId"]), "--repo", repo,
                                  "--name", CI_ARTIFACT, "--dir", str(folder)],
                                 capture_output=True, text=True, timeout=900)
        if fetched.returncode or not vpk.is_file():
            raise SystemExit(f"gh run download: {fetched.stderr.strip() or 'no KisakCOD-mp-test.vpk in the artifact'}")
    print(f"the build of {green[0]['headSha'][:12]} from CI run {green[0]['databaseId']}: {vpk}")
    return vpk


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

    eboot = sfo = head = None
    if args.ci_commit:
        args.vpk = str(ci_vpk(root, args.ci_commit, args.repo))
    if args.vpk:
        with zipfile.ZipFile(args.vpk) as package:
            eboot = package.read("eboot.bin")
            sfo = package.read("sce_sys/param.sfo")
        title = sfo_text(sfo, "TITLE_ID")
        if title != args.title or len(sfo) > MAX_SFO_BYTES:
            raise SystemExit(f"--vpk {args.vpk}: the package installs {title}, not the test title {args.title}; "
                             f"queue the build's KisakCOD-mp-test.vpk (or --eboot with its eboot.bin)")
        template = root / "head-template.bin"
        if template.is_file():
            head = make_head(template.read_bytes(), sfo)
        else:
            print(f"no head-template.bin (the head-template command): the Vita can only replace the build "
                  f"of a test title that is installed already")
            sfo = None
    elif args.eboot:
        eboot = Path(args.eboot).read_bytes()
    if eboot is not None and not 0 < len(eboot) <= MAX_EBOOT_BYTES:
        raise SystemExit("the eboot.bin is empty or over 64 MB")

    if args.pack:
        pack = Path(args.pack)
        if not pack.is_file() or not 0 < pack.stat().st_size <= MAX_PACK_BYTES or pack.read_bytes()[:8] != b"KGXPACK1":
            raise SystemExit(f"{pack}: not a program pack of up to 64 MB")

    # Made under a name the server skips, then renamed: a job is never
    # handed out half written.
    work = root / "queue" / f".new-{job_id}"
    (work / "files").mkdir(parents=True)
    for name, path in files.items():
        shutil.copyfile(path, work / "files" / name)
    if eboot is not None:
        (work / "eboot.bin").write_bytes(eboot)
    if sfo is not None and head is not None:
        (work / "param.sfo").write_bytes(sfo)
        (work / "head.bin").write_bytes(head)
    if args.pack:
        shutil.copyfile(args.pack, work / "programs.pack")
    if args.send:
        (work / "send.txt").write_text("\n".join(args.send) + "\n")
    work.rename(root / "queue" / job_id)
    build = f"its own build ({hashlib.sha256(eboot).hexdigest()[:12]})" if eboot is not None else "the installed client"
    print(f"queued {job_id}: {starts} starts on {build}, {len(files)} files" + (", a program pack" if args.pack else "")
          + (", a package to install the test title" if head is not None else ""))
    return 0


def command_head_template(args) -> int:
    root = Path(args.root)
    read_key(root)
    if args.source:
        data = Path(args.source).read_bytes()
    else:
        with urllib.request.urlopen(HEAD_TEMPLATE_URL, timeout=60) as answer:
            data = answer.read(1 << 16)
    if hashlib.sha256(data).hexdigest() != HEAD_TEMPLATE_SHA256:
        raise SystemExit("that is not the package head this expects (its SHA-256 differs)")
    (root / "head-template.bin").write_bytes(data)
    print(f"head-template.bin is in place ({len(data)} bytes): jobs from a test .vpk can install the test title")
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
    for name in ("key", "add", "serve", "list", "head-template"):
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
            build.add_argument("--ci-commit")
            sub.add_argument("--repo", default=CI_REPO)
            sub.add_argument("--file", action="append")
            sub.add_argument("--pack")
            sub.add_argument("--send", action="append")
            sub.add_argument("--title", default=TEST_TITLE, help="the test title's id (default %(default)s)")
        if name == "head-template":
            sub.add_argument("--from", dest="source")
    args = parser.parse_args()
    return {"key": command_key, "add": command_add, "serve": command_serve, "list": command_list,
            "head-template": command_head_template}[args.command](args)


if __name__ == "__main__":
    sys.exit(main())
