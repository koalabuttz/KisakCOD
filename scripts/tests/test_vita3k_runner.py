#!/usr/bin/env python3
"""Check what can be checked on the host of the Vita3K runner (scripts/runs/vita3k).

The runner itself is PowerShell for a Windows machine with Vita3K and WSL and
cannot run here. This checks the parts around it that can break without
anyone noticing until a run:

* ``vita_seal.py`` seals a results object with ``run_report.py``'s checksum,
  from the repository's layout (run_report.py one folder up) and from a
  machine's (beside it), and its results verify and render;
* the WSL scripts parse (``bash -n``);
* the JSON examples in the README parse and have the keys the runner requires;
* the runner names no machine, and every file the README lists is there.

Standard library only.

Usage:
    python3 scripts/tests/test_vita3k_runner.py
"""

import json
import re
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
RUNNER = ROOT / "scripts" / "runs" / "vita3k"
sys.path.insert(0, str(ROOT / "scripts" / "runs"))
import run_report  # noqa: E402

FAILURES = []


def check(ok, what):
    if not ok:
        FAILURES.append(what)
        print(f"  FAIL: {what}")


def body():
    return {
        "format": run_report.RESULT_FORMAT, "id": "self-test", "title": "A made-up Vita run", "asker": "the test",
        "machine": "a made-up machine", "started": "2026-11-10T00:00:00Z", "finished": "2026-11-10T00:02:00Z",
        "status": "ok", "compact_lines": 2,
        "build": {"asked": "abc", "commit": "0" * 40, "seconds": "17.1"},
        "rows": [{"name": "A-map", "variant": "map", "status": "ok", "exit": 0, "seconds": 74.9,
                  "values": {"ended": "by itself", "log": "whole"},
                  "lines": {"Vita memory": ["Vita memory now: one", "Vita memory now: two", "Vita memory now: three"]},
                  "counts": {"warnings": 0, "errors": 0},
                  "framelog": [{"label": "map", "line": "frame log map: 29 frames in 8.155 s: 3.56 fps; ms mean "
                                "281.213, median 48.657, p95 704.614, p99 707.882, max 707.882; 12 over 50 ms"}]}],
    }


def seal_in(folder, work, name):
    source = work / f"{name}-body.json"
    # As PowerShell writes it: with a byte order mark.
    source.write_text(json.dumps(body()), encoding="utf-8-sig")
    results, short = work / f"{name}-results.json", work / f"{name}-compact.json"
    done = subprocess.run([sys.executable, "-I", str(folder / "vita_seal.py"), str(source), str(results), str(short)],
                          capture_output=True, text=True, timeout=60)
    return done, results, short


def test_seal(work):
    print("vita_seal.py")
    done, results, short = seal_in(RUNNER, work, "repo")
    check(done.returncode == 0 and done.stdout.startswith("checksum ok"), f"seals from the repository's layout "
          f"({done.returncode}, {done.stdout!r}, {done.stderr!r})")
    if done.returncode == 0:
        sealed = run_report.load(results)
        check(run_report.verify(sealed) is None, "the sealed results verify")
        compact = run_report.load(short)
        check(run_report.verify(compact) is None and compact["full_sha256"] == sealed["sha256"],
              "the short form verifies and names the full file")
        check(compact["rows"][0]["lines"]["Vita memory"] == ["Vita memory now: one", "[1 more]", "Vita memory now: three"],
              "the short form carries the wanted lines the results ask for")
        check(len(short.read_text().splitlines()) == 1, "the short form is one line")
        text = run_report.render(compact)
        check("3.56" in text and "Vita memory now: one" in text, "the report reads a Vita run's short form")

    # A machine's layout: run_report.py beside the script.
    machine = work / "tools"
    machine.mkdir()
    shutil.copy(RUNNER / "vita_seal.py", machine)
    shutil.copy(ROOT / "scripts" / "runs" / "run_report.py", machine)
    done, results, _ = seal_in(machine, work, "machine")
    check(done.returncode == 0 and run_report.verify(run_report.load(results)) is None,
          f"seals from a machine's layout ({done.returncode}, {done.stderr!r})")

    bad = work / "bad.json"
    bad.write_text(json.dumps({"format": "something else"}))
    done = subprocess.run([sys.executable, "-I", str(RUNNER / "vita_seal.py"), str(bad), str(work / "x"), str(work / "y")],
                          capture_output=True, text=True, timeout=60)
    check(done.returncode == 2 and not (work / "x").exists(), "refuses an object of another format")


def test_scripts():
    print("the scripts")
    scripts = sorted((RUNNER / "wsl").glob("*.sh"))
    check(len(scripts) == 3, "the three WSL scripts are there")
    bash = shutil.which("bash")
    for script in scripts:
        if bash:
            done = subprocess.run([bash, "-n", str(script)], capture_output=True, text=True, timeout=60)
            check(done.returncode == 0, f"{script.name} parses ({done.stderr.strip()})")
        check("\r" not in script.read_text(), f"{script.name} has Unix line ends")
    runner = (RUNNER / "vita-run.ps1").read_text()
    check("machine = $machine" in runner and "machine.txt" in runner, "the runner takes the machine's name from the machine")
    for word in ("C:\\Users", "Lewis"):
        check(word not in runner, f"the runner does not name {word}")
    check("The app exited with code" in runner, "the runner ends a start at Vita3K's exit line")


def test_readme():
    print("the README")
    readme = (RUNNER / "README.md").read_text()
    examples = re.findall(r"\n((?:    .*\n|\n)+)", readme)
    parsed = []
    for block in examples:
        text = "\n".join(line[4:] for line in block.splitlines())
        if not text.lstrip().startswith("{"):
            continue
        try:
            parsed.append(json.loads(text))
        except ValueError:
            continue
    runs = [example for example in parsed if isinstance(example, dict) and "starts" in example]
    check(len(runs) >= 2, f"the README's two example descriptions parse ({len(runs)})")
    for example in runs:
        check(re.fullmatch(r"[A-Za-z0-9_.+-]+", example.get("id", "")) is not None and "commit" in example
              and all(re.fullmatch(r"[A-Za-z0-9_.+-]+", start.get("tag", "")) and "cmdline" in start
                      for start in example["starts"]), f"example {example.get('id')} has what the runner requires")
    listed = re.findall(r"^    ([\w./-]+\.(?:ps1|py|sh))\b(.*)$", readme, re.M)
    check(len(listed) >= 5, "the README lists the files")
    for name, rest in listed:
        if "not here yet" in rest or name == "run_report.py":
            continue
        check((RUNNER / name).is_file(), f"{name}, which the README lists, is in the folder")


def main() -> int:
    with tempfile.TemporaryDirectory(prefix="kisak-vita3k-runner-") as work:
        test_seal(Path(work))
    test_scripts()
    test_readme()
    if FAILURES:
        print(f"FAIL: {len(FAILURES)} check(s) failed")
        return 1
    print("PASS: Vita3K runner files")
    return 0


if __name__ == "__main__":
    sys.exit(main())
