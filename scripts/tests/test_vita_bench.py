#!/usr/bin/env python3
"""Check the PS Vita bench that fetches its work, on the host.

A Vita with ``bench.txt`` on its card asks a computer for jobs: a build, a
run list and step files; it runs the list and sends the logs back
(docs/vita.md, "A bench that fetches its work"). The Vita's side
(``src/vita/vita_bench.h``, ``vita_bench_client.h``) is written with calls
the host has too, so this builds ``tests/vita_client/bench_test.cpp`` with
the host compiler (ASan and UBSan on) and runs it against the real server
script, ``scripts/platform/vita/vita_bench_server.py``, with temporary folders for the
card and the computer. The test plays the system: where the Vita's side
starts the other build, it runs the next process.

Checked: the formats and codes; a job on a fetched build and one on the
installed build, start by start, with every log back on the computer; a card
with the wrong key gets nothing; a job description that was not signed with
the key, and a file that is not the one the job named, are not used; a file
the computer does not give leaves the job waiting; a bad
run list, a fetched build that cannot be started or never comes up, and a
start that faults each end as the documentation says.

Usage:
    python3 scripts/tests/test_vita_bench.py [--cxx c++] [--timeout 300]
"""

import argparse
import hashlib
import importlib.util
import os
import shutil
import subprocess
import sys
import tempfile
import threading
import time
import zipfile
from http.server import HTTPServer
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
SOURCE = ROOT / "tests" / "vita_client" / "bench_test.cpp"
SERVER = ROOT / "scripts" / "platform" / "vita" / "vita_bench_server.py"

failures = 0


def check(condition, what):
    global failures
    if not condition:
        failures += 1
        print(f"FAIL: {what}")


class Card:
    """The Vita's side: its data folder, the fetched build's place, and the system that restarts it."""

    def __init__(self, work: Path, name: str, binary: Path, timeout: int):
        self.data = work / name / "data"
        self.eboot = work / name / "app" / "eboot-bench.bin"
        self.data.mkdir(parents=True)
        self.eboot.parent.mkdir(parents=True)
        self.binary = binary
        self.timeout = timeout

    def point_at(self, port: int, key: str, wait: int = 0):
        (self.data / "bench.txt").write_text(f"127.0.0.1 {port} {key}" + (f" wait {wait}" if wait else "") + "\n")

    def start(self, fetched=False, by_bench=False, **environment):
        """One process. Returns (exit code, output)."""
        label = "the fetched build " + hashlib.sha256(self.eboot.read_bytes()).hexdigest()[:8] if fetched \
            else "the installed build"
        run = subprocess.run(
            [str(self.binary), "start", str(self.data), str(self.eboot), str(int(fetched)), str(int(by_bench)), label],
            capture_output=True, text=True, timeout=self.timeout, env={**os.environ, **environment})
        return run.returncode, run.stdout + run.stderr

    def tap(self, **environment):
        """A tap on the bubble, and every restart the bench asks for after it."""
        code, output = self.start(**environment)
        text = output
        for _ in range(40):
            if code not in (10, 11):
                break
            code, output = self.start(fetched=code == 11, by_bench=True)
            text += output
        return code, text


def load_server_module():
    spec = importlib.util.spec_from_file_location("vita_bench_server", SERVER)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def server_command(*arguments, timeout=60):
    run = subprocess.run([sys.executable, str(SERVER), *arguments], capture_output=True, text=True, timeout=timeout)
    return run.returncode, run.stdout + run.stderr


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--cxx", default=shutil.which("c++") or "g++")
    parser.add_argument("--timeout", type=int, default=300)
    args = parser.parse_args()

    with tempfile.TemporaryDirectory(prefix="kisak-vita-bench-") as temporary:
        work = Path(temporary)
        binary = work / "bench_test"
        command = [args.cxx, "-std=c++20", "-O1", "-g", "-Wall", "-Wextra", "-Werror",
                   "-fsanitize=address,undefined", "-fno-sanitize-recover=all",
                   "-I", str(ROOT / "src"), str(SOURCE), "-o", str(binary)]
        build = subprocess.run(command, capture_output=True, text=True, timeout=args.timeout)
        if build.returncode:
            sys.stdout.write(build.stdout + build.stderr)
            print("FAIL: the bench test did not build")
            return 1
        run = subprocess.run([str(binary)], capture_output=True, text=True, timeout=args.timeout)
        sys.stdout.write(run.stdout + run.stderr)
        if run.returncode:
            print("FAIL: vita bench format checks failed")
            return 1

        # ---- The computer: its folder, key and server.
        root = work / "computer"
        code, output = server_command("key", "--root", str(root))
        check(code == 0 and "bench.txt" in output, f"the key command: {output}")
        key = (root / "key.txt").read_text().strip()
        check(len(key) == 32 and output.strip().endswith(key), "the key command prints the card's line")
        check((root / "key.txt").stat().st_mode & 0o077 == 0, "key.txt is the owner's alone")
        server = subprocess.Popen([sys.executable, str(SERVER), "serve", "--root", str(root), "--bind", "127.0.0.1",
                                   "--port", "0"], stdout=subprocess.PIPE, text=True)
        try:
            port = int(server.stdout.readline().split("listening on 127.0.0.1:")[1].split(",")[0])
            failed = scenarios(work, root, port, key, binary, args.timeout)
        finally:
            server.terminate()
            server.wait(timeout=30)
        if failed or failures:
            print("FAIL: vita bench")
            return 1
    print("PASS: vita bench")
    return 0


def done_job(root: Path, name: str):
    found = [job for job in (root / "done").iterdir() if job.name.endswith("-" + name)]
    return found[0] if len(found) == 1 else None


def scenarios(work: Path, root: Path, port: int, key: str, binary: Path, timeout: int) -> int:
    sources = work / "sources"
    sources.mkdir()

    # ---- Nothing queued: a tap is an ordinary start; without bench.txt the
    # bench does nothing at all.
    card = Card(work, "card", binary, timeout)
    code, output = card.tap()
    check(code == 0 and output.strip() == "ORDINARY" and not (card.data / "bench.log").exists(),
          f"no bench.txt: {code} {output}")
    card.point_at(port, key)
    code, output = card.tap()
    check(code == 0 and "no job waits; this is an ordinary start" in output, f"no job: {code} {output}")

    # ---- Two jobs: the first on a build of its own (from a .vpk), the
    # second on the installed build.
    build_one = os.urandom(200 * 1024 + 123)
    with zipfile.ZipFile(sources / "one.vpk", "w") as package:
        package.writestr("sce_sys/param.sfo", b"sfo")
        package.writestr("eboot.bin", build_one)
    (sources / "list-one.txt").write_text(
        "# two starts\nA1 | - | +devmap mp_crash +set vita_quitAfterMap 20\nA2 | no-vsync | +set vita_quitAfterMenu 5\n")
    (sources / "run-a.txt").write_text("wait 5\nvita_gxm no-vsync 1\n")
    (sources / "empty.txt").write_text("")
    (sources / "list-two.txt").write_text("B1 | - | +set vita_quitAfterMenu 5\n")
    pack = b"KGXPACK1" + os.urandom(300 * 1024)
    (sources / "programs.pack").write_bytes(pack)
    code, output = server_command("add", "--root", str(root), "--name", "one", "--vpk", str(sources / "one.vpk"),
                                  "--pack", str(sources / "programs.pack"),
                                  "--run-list", str(sources / "list-one.txt"), "--file", str(sources / "run-a.txt"),
                                  "--file", str(sources / "empty.txt"), "--send", "extra.json", "--send", "absent.csv")
    check(code == 0 and "2 starts on its own build" in output and "a program pack" in output, f"add one: {output}")
    time.sleep(1.1)  # a job's name starts with the second it was queued
    code, output = server_command("add", "--root", str(root), "--name", "two", "--run-list", str(sources / "list-two.txt"))
    check(code == 0 and "1 starts on the installed build" in output, f"add two: {output}")
    for bad in (["--name", "x", "--run-list", str(sources / "list-one.txt"), "--file", str(sources / "one.vpk")],
                ["--name", "x", "--run-list", str(sources / "run-a.txt")],
                ["--name", "x", "--run-list", str(sources / "empty.txt")],
                ["--name", "../x", "--run-list", str(sources / "list-one.txt")],
                ["--name", "x", "--run-list", str(sources / "list-one.txt"), "--pack", str(sources / "one.vpk")],
                ["--name", "x", "--run-list", str(sources / "list-one.txt"), "--send", "bench.txt"]):
        code, output = server_command("add", "--root", str(root), *bad)
        check(code != 0, f"add {bad} should be refused: {output}")

    (card.data / "extra.json").write_text('{"frames": 3}\n')
    code, output = card.tap()
    check(code == 2 and output.strip().endswith("QUIT"), f"two jobs, then the process ends: {code} {output}")
    check(card.eboot.read_bytes() == build_one, "the fetched build is the one queued")
    check((card.data / "gxp" / "programs.pack").read_bytes() == pack, "the program pack is in the program folder")
    fetched_label = "the fetched build " + hashlib.sha256(build_one).hexdigest()[:8]
    order = [line for line in output.splitlines() if line.startswith(("START", "NEXT"))]
    check(order == ["NEXT fetched", f"START A1 on {fetched_label}", "NEXT fetched", f"START A2 on {fetched_label}",
                    "NEXT installed", "START B1 on the installed build", "NEXT installed"], f"the order: {order}")
    one, two = done_job(root, "one"), done_job(root, "two")
    check(one is not None and two is not None and not list((root / "queue").iterdir()), "both jobs are done")
    if one and two:
        check((one / "result.txt").read_text() == "ok\n" and (two / "result.txt").read_text() == "ok\n", "results")
        check(sorted(path.name for path in (one / "logs").iterdir()) ==
              ["bench.log", "console-mp-A1.log", "console-mp-A2.log", "extra.json"], "job one's logs")
        check((one / "logs" / "console-mp-A2.log").read_text() ==
              f"start A2 on {fetched_label}: +set vita_quitAfterMenu 5\n", "a log's text")
        check((one / "logs" / "extra.json").read_text() == '{"frames": 3}\n', "a file asked back")
        check("fetched the build" in (one / "logs" / "bench.log").read_text(), "the bench's own log")
        check(sorted(path.name for path in (two / "logs").iterdir()) == ["bench.log", "console-mp-B1.log"],
              "job two's logs")
        check((two / "logs" / "console-mp-B1.log").read_text().startswith("start B1 on the installed build"),
              "job two ran on the installed build")
    check((card.data / "run-a.txt").read_text() == "wait 5\nvita_gxm no-vsync 1\n" and
          (card.data / "empty.txt").read_text() == "", "the step files are on the card")
    check(not (card.data / "runlist-mp.txt").exists() and not (card.data / "bench-job.txt").exists() and
          not list(card.data.glob("*.part")), "the card is left with no job")
    code, output = server_command("list", "--root", str(root))
    check(code == 0 and output.count("done ") == 2 and "ok" in output, f"list: {output}")

    # ---- The same build again is not fetched twice; a person ends a start
    # (START + SELECT): the chain stops, the next tap goes on.
    code, output = server_command("add", "--root", str(root), "--name", "three", "--eboot", str(card.eboot),
                                  "--run-list", str(sources / "list-one.txt"))
    check(code == 0, f"add three: {output}")
    code, output = card.start()
    check(code == 11 and "is on the card already" in "".join((card.data / "bench.log").read_text().splitlines()[-3:]),
          f"the build is kept: {code} {output}")
    code, output = card.start(fetched=True, by_bench=True, BENCH_TEST_STOPPED="1")
    check(code == 3 and "START A1" in output, f"a start ended by hand: {code} {output}")
    check(not (root / "done").joinpath("x").exists() and done_job(root, "three") is None, "the job is not over yet")
    # ---- ... and the last start faults: the next tap sends what there is.
    code, output = card.start()
    check(code == 11, f"the tap goes on with the job: {code} {output}")
    code, output = card.start(fetched=True, by_bench=True, BENCH_TEST_START_FAULTS="1")
    check(code == 5 and "START A2" in output, f"a start that faults: {code} {output}")
    code, output = card.tap()
    three = done_job(root, "three")
    check(code == 0 and three is not None and (three / "result.txt").read_text() == "ok\n",
          f"after the fault the logs go back: {code} {output}")
    if three:
        check({"console-mp-A1.log", "console-mp-A2.log"} <= {path.name for path in (three / "logs").iterdir()},
              "both starts' logs of job three")

    # ---- A fetched build that cannot be started, and one that never comes
    # up: the job fails, the list does not run on the installed build.
    build_two = os.urandom(70 * 1024)
    (sources / "two.bin").write_bytes(build_two)
    code, output = server_command("add", "--root", str(root), "--name", "four", "--eboot", str(sources / "two.bin"),
                                  "--run-list", str(sources / "list-two.txt"))
    code, output = card.tap(BENCH_TEST_FETCHED_DOES_NOT_START="1")
    four = done_job(root, "four")
    check(code == 0 and "START" not in output and four is not None, f"a build that cannot be started: {code} {output}")
    if four:
        check((four / "result.txt").read_text() == "failed: the fetched build could not be started\n", "four's result")
    check(not (card.data / "runlist-mp.txt").exists(), "its list is set aside")

    code, output = server_command("add", "--root", str(root), "--name", "five", "--eboot", str(sources / "two.bin"),
                                  "--run-list", str(sources / "list-two.txt"))
    code, output = card.start()
    check(code == 11, f"five: the first try: {code} {output}")
    code, output = card.start()  # the build did not come up; the person taps again
    check(code == 11 and "try 2" in (card.data / "bench.log").read_text().splitlines()[-1],
          f"five: the second try: {code} {output}")
    code, output = card.tap()
    five = done_job(root, "five")
    check(code == 0 and "START" not in output and five is not None, f"a build that never comes up: {code} {output}")
    if five:
        check((five / "result.txt").read_text() == "failed: the fetched build did not come up, tried twice\n",
              "five's result")
    check(not (card.data / "runlist-mp.txt").exists(), "its list is set aside too")

    # ---- A run list the Vita refuses: the job fails before anything is put
    # in place.
    (sources / "list-bad.txt").write_text("C1 | NOT_A_MARKER | +set vita_quitAfterMenu 5\n")
    code, output = server_command("add", "--root", str(root), "--name", "six", "--run-list", str(sources / "list-bad.txt"))
    check(code == 0, f"add six: {output}")
    code, output = card.tap()
    six = done_job(root, "six")
    check(code == 0 and "START" not in output and six is not None, f"a bad run list: {code} {output}")
    if six:
        check((six / "result.txt").read_text().startswith("failed: run list line 1:"), "six's result")
        # The card still holds job two's list and log; a job that failed
        # before its files were in place sends no start's log (console run
        # AB sent run AA's).
        sent = sorted(p.name for p in (six / "logs").iterdir()) if (six / "logs").is_dir() else []
        check((card.data / "console-mp-B1.log").exists() and sent == ["bench.log"],
              f"six sent only its bench.log: {sent}")
    check(not (card.data / "runlist-mp.txt").exists() and not list(card.data.glob("*.part")), "nothing was put in place")

    # ---- A card with another key gets nothing, and the job stays queued.
    code, output = server_command("add", "--root", str(root), "--name", "seven", "--run-list", str(sources / "list-two.txt"))
    stranger = Card(work, "stranger", binary, timeout)
    stranger.point_at(port, "0123456789abcdef0123456789abcdef")
    code, output = stranger.tap()
    check(code == 0 and "the computer answered 403" in output and "START" not in output, f"another key: {code} {output}")
    check(not (stranger.data / "runlist-mp.txt").exists() and len(list((root / "queue").iterdir())) == 1,
          "nothing reached the stranger")

    # ---- A computer that does not hold the key, and one whose file is not
    # the file the job names: neither is used.
    module = load_server_module()

    class Impostor(module.Handler):
        def answer(self, status, body=b"", mac=""):
            if self.path.startswith("/bench/job?"):
                mac = module.code("not the key at all 123", b"job", self.path.split("nonce=")[1].encode(), body)
            super().answer(status, body, mac)

    class Dropper(module.Handler):
        def answer(self, status, body=b"", mac=""):
            if self.path.startswith("/bench/blob/"):
                status, body = 503, b""
            super().answer(status, body, mac)

    class Swapper(module.Handler):
        def answer(self, status, body=b"", mac=""):
            if self.path.startswith("/bench/blob/") and body:
                body = body[:-1] + bytes([body[-1] ^ 1])
            super().answer(status, body, mac)

    for handler, expected in ((Impostor, "the job's code was not made with the key"),
                              (Dropper, "the computer answered 503; it is asked for again"),
                              (Swapper, "bytes that are not that file")):
        other = HTTPServer(("127.0.0.1", 0), handler)
        other.root, other.key, other.handed = root, key, None
        thread = threading.Thread(target=other.serve_forever, daemon=True)
        thread.start()
        try:
            victim = Card(work, handler.__name__, binary, timeout)
            victim.point_at(other.server_address[1], key)
            code, output = victim.start()
            check(code == 0 and expected in output and "START" not in output, f"{handler.__name__}: {code} {output}")
            check(not (victim.data / "runlist-mp.txt").exists() and not list(victim.data.glob("*.part")),
                  f"{handler.__name__}: nothing was put in place")
            if handler is not Swapper:
                check(len(list((root / "queue").iterdir())) == 1, f"{handler.__name__}: the job still waits")
        finally:
            other.shutdown()
            other.server_close()
    # The swapped file failed job seven on that card, which said so.
    seven = done_job(root, "seven")
    check(seven is not None and (seven / "result.txt").read_text().startswith("failed: fetching"), "seven's result")

    # ---- No computer there: a tap waits as long as bench.txt says, a held
    # START + SELECT ends the wait.
    silent = Card(work, "silent", binary, timeout)
    silent.point_at(1, key, wait=10)
    code, output = silent.tap()
    check(code == 0 and output.count("WAIT") == 2 and "the computer does not answer" in output,
          f"no computer: {code} {output}")
    code, output = silent.tap(BENCH_TEST_STOP_WAIT="1")
    check(code == 0 and output.count("WAIT") == 1 and "ended by hand" in output, f"a wait ended by hand: {code} {output}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
